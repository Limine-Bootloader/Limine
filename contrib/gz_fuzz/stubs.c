/*  Stubs for the Limine APIs to work in a POSIX userland.  */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <setjmp.h>

#include <fs/file.h>
#include <mm/pmm.h>
#include <lib/misc.h>

jmp_buf fuzz_panic_jmp;
int fuzz_panic_armed = 0;

__attribute__((noreturn))
void panic(bool nostack, const char * fmt, ...) {
  if (fuzz_panic_armed)
    longjmp(fuzz_panic_jmp, 1);
  static const char msg[] = "stubs: unexpected panic outside harness\n";
  write(2, msg, sizeof(msg) - 1);
  abort();
}

#define FUZZ_MAX_ALLOCS 1024
static void * fuzz_allocs[FUZZ_MAX_ALLOCS];
static size_t fuzz_alloc_count = 0;

void * ext_mem_alloc(uint64_t count) {
  if (fuzz_alloc_count >= FUZZ_MAX_ALLOCS)
    panic(false, "stubs: allocation table full");
  void * p = calloc(1, (size_t)count);
  if (!p) panic(false, "stubs: out of host memory");
  fuzz_allocs[fuzz_alloc_count++] = p;
  return p;
}

void pmm_free(void *ptr, uint64_t length) {
  if (ptr == NULL) return;
  for (size_t i = 0; i < fuzz_alloc_count; i++) {
    if (fuzz_allocs[i] == ptr) {
      fuzz_allocs[i] = fuzz_allocs[--fuzz_alloc_count];
      free(ptr);  return;
    }
  }
  abort();  /*  Free of untracked pointer.  */
}

void fuzz_drain_allocs(void) {
  for (size_t i = 0; i < fuzz_alloc_count; i++)
    free(fuzz_allocs[i]);
  fuzz_alloc_count = 0;
}

/*  Copied from Limine.  */
void fread(struct file_handle * fd, void * buf, uint64_t loc, uint64_t count) {
  if (fd->is_memfile) {
    if (loc > fd->size || count > fd->size - loc)
      panic(false, "stubs: memfile read out of bounds");
    memcpy(buf, (uint8_t *)fd->fd + loc, (size_t)count);
    return;
  }
  fd->read(fd, buf, loc, count);
}

void fclose(struct file_handle * fd) { }
