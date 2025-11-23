.POSIX:

SHELL=/bin/sh

CC=cc
CFLAGS=-g -O2 -pipe
CPPFLAGS=
LDFLAGS=
LIBS=

.PHONY: all
all: limine

.PHONY: clean
clean:
	rm -f limine limine.exe

limine: limine.c
	$(CC) $(CFLAGS) -std=c99 $(CPPFLAGS) $(LDFLAGS) $< $(LIBS) -o $@
