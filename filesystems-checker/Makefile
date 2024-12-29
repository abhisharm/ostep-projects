CC = gcc
CFLAGS = -Wall -g

all: xcheck

xcheck: xcheck.c fs.h
	$(CC) $(CFLAGS) -o xcheck xcheck.c

# todo: add some testing stuff
run: xcheck
	./xcheck fs.img

clean:
	rm -f xcheck
