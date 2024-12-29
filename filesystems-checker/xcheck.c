#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "fs.h"
#include "param.h"

#define T_DIR  1   // Directory
#define T_FILE 2   // File
#define T_DEV  3   // Device

typedef unsigned char uchar;

FILE *fp;
struct superblock sb;
uint ninodes, iblocks, bmapblocks, nblocks, bmapstart, nmeta;
uint *inode_refs, *block_refs;
struct dinode *inodes;

// functions for reading in blocks and data-structures (inodes)
// from the disk image.
void fp_seek_block(uint num);
void fp_seek_inode(uint inum);
void fp_seek(uint off, int whence); 
void rblock(uint num, void *buf);
void rinode(uint inum, struct dinode *ip);
void rdirent(uint ent, void *buf, struct dirent *ep);
uint bmapval(uint num);

// recursively traverse through a directory, filling out the bookkeeping
// data-structures.
void recurse_dir(struct dinode *dir, uint inum);
void process_file(struct dinode *f);

// helper functions
uint check_bp(uint bp);


int main(int argc, char** argv) {
	if (argc != 2) {
		printf("usage: xcheck <filesystem_image>\n");
		return 1;
	}

 	fp = fopen(argv[1], "rb");
	if (fp == NULL) {
		printf("invalid image file.\n");
	} 

	struct dinode root_inode;
	// The superblock should be one block after the boot block.
	fp_seek(BSIZE, SEEK_CUR);
	if (fread(&sb, sizeof(struct superblock), 1, fp) != 1) {
 		printf("Unable to read superblock.\n");
	}
	bmapblocks = sb.size / (BSIZE*8) + 1;
	ninodes = sb.ninodes;
	iblocks = ninodes / IPB + 1;
	// metadata includes boot block, superblock (2), inode & bmap blocks
	nmeta = 2 + sb.nlog + iblocks + bmapblocks;
	nblocks = sb.nblocks;
	bmapstart = sb.bmapstart;
	inode_refs = malloc(sb.ninodes * BSIZE);
	block_refs = malloc(nblocks * BSIZE);
	memset(inode_refs, 0, sb.ninodes * BSIZE);
	memset(block_refs, 0, nblocks * BSIZE);

	// Find the root directory inode and start traversing it
	// and building the maps.
	rinode(1, &root_inode);
	if (root_inode.type != T_DIR) {
		printf("ERROR: root directory does not exist.\n");
		exit(1);
	}

	// check that root inode is its own parent
	char buf[BSIZE];
	struct dirent parent_dirent;
	rblock(root_inode.addrs[0], buf);
	rdirent(1, buf, &parent_dirent);
	if (parent_dirent.inum != 1) {
		printf("ERROR: root directory does not exist.\n");
		exit(1);
	}
	// start recursive traversal from the root to build out maps.
	recurse_dir(&root_inode, 1);

	// iterate over all inodes and check them.
	struct dinode inode;
	for (int i = 2; i < ninodes; ++i) {
		rinode(i, &inode);
		if (inode.type == 0) {
			if (inode_refs[i] != 0) {
				printf("ERROR: inode referred "
				       "to in directory but marked free.");
				exit(1);
			} else {
				continue;
			}
		}

		if (inode.type < T_DIR || inode.type > T_DEV) {
			printf("ERROR: bad inode.");
			exit(1);
		}

		if (inode_refs[i] == 0) {
			printf("ERROR: inode marked use but "
			       "not found in a directory.");
			printf("\ninode number: %d", i);
			exit(1);
		}

		if (inode.type == T_DIR &&
		    inode_refs[i] > 1) {
			printf("ERROR: directory appears "
			       "more than once in file system.");
			exit(1);
		}

		if (inode.nlink != inode_refs[i]){
			printf("ERROR: bad reference count for file.");
			exit(1);
		}

		for (int i = 0; i < NDIRECT; i++) {
			if (inode.addrs[i] == 0) {
				break;
			}
			if (!bmapval(inode.addrs[i])) {
				printf("ERROR: address used by inode "
				       "but marked free in bitmap.");
				exit(1);
			}
			if (i == NDIRECT - 1) {
				if (inode.addrs[NDIRECT] == 0){
					break;
				}
				if (!bmapval(inode.addrs[NDIRECT])) {
					printf("ERROR: address used by "
					       "inode but marked free in bitmap.");
				}
			}
		}
	}

	// iterate over the bitmap and and check each entry.
	for (int i = nmeta; i < nblocks; ++i) {
		if (bmapval(i) && block_refs[i - nmeta] == 0) {
			printf("ERROR: bitmap marks block in "
			       "use but it is not in use.");
			exit(1);
		}
	}

	fclose(fp);
	return 0;
}

void recurse_dir(struct dinode *dir, uint inum) {
	if (dir->type != T_DIR) {
		printf("Application Error: 'traversedir'"
		       " called on a non-directory");
		exit(1);
	}

	uint bp, bdeidx, maxidx;
	char buf[BSIZE];
	struct dirent de;
	struct dinode cur_inode;
	maxidx = BSIZE / sizeof(struct dirent);
	for (int i = 0; i < NDIRECT; ++i) {
		bdeidx = 0;
		bp = dir->addrs[i];
		if (bp == 0) {
			break;
		}
		if (!check_bp(bp)) {
			printf("ERROR: bad direct address in inode.");
			exit(1);
		}
		// need to mark directory data-block as referenced.
		++block_refs[bp - nmeta];
		rblock(bp, buf);
		// first check that the '.' and '..' dirs
		// are correct.
		if (i == 0) {
			rdirent(bdeidx++, buf, &de);
			if (strcmp(de.name, ".") != 0
			    || de.inum != inum) {
				printf("ERROR: directory not properly formatted.");
				exit(1);
			}

			rdirent(bdeidx++, buf, &de);
			if (strcmp(de.name, "..") != 0) {
				printf("ERROR: directory not properly formatted.");
				exit(1);
			}
		}
		while (bdeidx < maxidx) {
			rdirent(bdeidx++, buf, &de);
			// processed all dirents.
			if (de.inum == 0) {
				break;
			}
			rinode(de.inum, &cur_inode);
			if (cur_inode.type < T_DIR ||
			    cur_inode.type > T_DEV) {
				printf("Invalide inode type\n");
				exit(1);
			}

			// update bookkeeping for inode depending on type.
			++inode_refs[de.inum];
			if (cur_inode.type == T_DIR) {
				if (inode_refs[de.inum] > 1) {
					printf("ERROR: directory appears more "
					       "than once in file system.");
					exit(1);
				}
				recurse_dir(&cur_inode, de.inum);
			} else {
				process_file(&cur_inode);
			}
		}
	}
	// I'm pretty sure we don't need to scan the indirect block of
	// a directory. We would run out of inodes before we get to that
	// point. (200 inodes, IPB = 25(?), NDIRECT=12, 12 * 25 > 200).
	// Although it might be necessary if deletions are allowed.
}

void process_file(struct dinode *f) {
	if (f->type != T_FILE && f->type != T_DEV) {
		printf("Application Error: non-file inode"
		       " passed to process_file");
		exit(1);
	}

	uint bp;
	for (int i = 0; i < NDIRECT; ++i) {
		bp = f->addrs[i];
		if (bp == 0) {
			return;
		}
		if (!check_bp(bp)) {
			printf("ERROR: bad direct address in inode.");
			exit(1);
		}
		if(++block_refs[bp - nmeta] > 1) {
			printf("ERROR: direct address used more than once.");
			exit(1);
		}
	}

	uint ibp = f->addrs[NDIRECT];
	if (ibp == 0) {
		return;
	}
	if (!check_bp(bp)) {
		printf("ERROR: bad direct address in inode.");
		exit(1);
	}
	// need to also record the usage of indirect block.
	if (++block_refs[ibp - nmeta] > 1) {
		printf("ERROR: direct address used more than once.");
	}
	uint *bpi;
	char buf[BSIZE];
	rblock(ibp, buf);
	for (int i = 0; i < NINDIRECT; ++i) {
		bpi = (uint *)buf + i;
		if (*bpi == 0) {
			return;
		}
		if (!check_bp(*bpi)) {
			printf("ERROR: bad indirect address in inode.");
			exit(1);
		}
		if (++block_refs[*bpi - nmeta] > 1) {
			printf("ERROR: indirect address used more than once.");
			exit(1);
		}
	}
}

void fp_seek_block(uint num) {
	uint off;
	off = num * BSIZE;
	fp_seek(off, SEEK_SET);
}

void fp_seek(uint off, int whence) {
	if (fseek(fp, off, whence) < 0) {
		perror("fseek");
		exit(1);
	}
}

void rblock(uint num, void *buf) {
	fp_seek_block(num);
	if (fread((char *)buf, BSIZE, 1, fp) != 1) {
		perror("fread");
		exit(1);
	}
}

void rinode(uint inum, struct dinode *ip) {
	char buf[BSIZE];
	uint bn;
	struct dinode *dip;

	bn = IBLOCK(inum, sb);
	rblock(bn, buf);
	dip = (struct dinode*)buf + (inum % IPB);
	*ip = *dip;
}

void rdirent(uint ent, void *buf, struct dirent *ep) {
	struct dirent *dep;
	dep = (struct dirent*)buf + ent;
	*ep = *dep;
}

uint bmapval(uint num) {
	uint blk, bit;
	uchar byte;
	uchar buf[BSIZE];
	blk = (num / 8) / BSIZE;
	bit = num - (BSIZE * 8 * blk);
	rblock(blk + bmapstart, buf);
	byte = buf[bit/8];
	uint retval = (byte >> (bit%8)) & 0x1;
	return retval;
}

uint check_bp(uint bp) {
	return bp >= nmeta && bp < sb.nblocks;
}
	

