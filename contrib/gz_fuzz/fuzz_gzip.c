/*  AFL++ harness for common/compress/gzip.c
    Feeds arbitrary bytes as a fake memfile-backed struct file_handle into
    gzip_check()/gzip_open() and drives the read callback until EOF or panic.
    Build with afl-clang-fast or afl-clang-lto; see the Makefile.
    Falls back to a stdin-driver binary when built with a "plain" compiler.  */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <setjmp.h>

#include <fs/file.h>
#include <compress/gzip.h>

extern jmp_buf fuzz_panic_jmp;
extern int fuzz_panic_armed;
void fuzz_drain_allocs(void);

#define MAX_INPUT     (4 * 1024 * 1024)
#define MAX_OUTPUT    (16 * 1024 * 1024)
#define CHUNK_BYTES   65536

static uint8_t out_chunk[CHUNK_BYTES];

static void run_one(const uint8_t * data, size_t size) {
  if (size < 18) return;
  struct file_handle src;
  memset(&src, 0, sizeof(src));
  src.is_memfile = true;
  src.fd         = (void *)(uintptr_t)data;
  src.size       = size;
  fuzz_panic_armed = 1;
  if (setjmp(fuzz_panic_jmp) != 0) {
    fuzz_panic_armed = 0;
    fuzz_drain_allocs();
    return;
  }
  if (!gzip_check(&src)) {
    fuzz_panic_armed = 0;  return;
  }
  struct file_handle * dec = gzip_open(&src);
  if (dec == NULL) {
    fuzz_panic_armed = 0;
    fuzz_drain_allocs();
    return;
  }
  uint64_t total = dec->size, n;
  if (total > MAX_OUTPUT) total = MAX_OUTPUT;
  for (uint64_t pos = 0; pos < total; pos += n) {
    if ((n = total - pos) > sizeof(out_chunk)) n = sizeof(out_chunk);
    fread(dec, out_chunk, pos, n);
  }
  fuzz_panic_armed = 0;
  fuzz_drain_allocs();
}

#ifdef __AFL_COMPILER
__AFL_FUZZ_INIT();
#endif

int main(void) {
#ifdef __AFL_COMPILER
  __AFL_INIT();
  unsigned char * buf = __AFL_FUZZ_TESTCASE_BUF;
  while (__AFL_LOOP(10000)) {
    int len = __AFL_FUZZ_TESTCASE_LEN;
    if (len < 0) len = 0;
    if ((size_t)len > MAX_INPUT) len = MAX_INPUT;
    run_one(buf, (size_t)len);
  }
#else
  static uint8_t buf[MAX_INPUT];
  size_t total = 0;
  for (;;) {
    ssize_t n = read(0, buf + total, sizeof(buf) - total);
    if (n <= 0) break;
    total += (size_t)n;
    if (total == sizeof(buf)) break;
  }
  run_one(buf, total);
#endif
}
