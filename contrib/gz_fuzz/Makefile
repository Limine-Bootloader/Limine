LIMINE_ROOT ?= ../..
GZIP_SRC    := $(LIMINE_ROOT)/common/compress/gzip.c
PDGZIP_SRC  := $(LIMINE_ROOT)/common/compress/pdgzip.c
CC       ?= afl-clang-fast
CSTD     ?= -std=c99
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CPPFLAGS := -I./include -I$(LIMINE_ROOT)/common
OBJS := fuzz_gzip.o stubs.o gzip.o pdgzip.o
all: fuzz_gzip
fuzz_gzip: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)
gzip.o: $(GZIP_SRC)
	$(CC) $(CFLAGS) $(CSTD) $(CPPFLAGS) -c -o $@ $<
pdgzip.o: $(PDGZIP_SRC)
	$(CC) $(CFLAGS) -std=c11 $(CPPFLAGS) -c -o $@ $<
%.o: %.c
	$(CC) $(CFLAGS) $(CSTD) $(CPPFLAGS) -c -o $@ $<
clean:
	rm -f $(OBJS) fuzz_gzip
.PHONY: all clean
