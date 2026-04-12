#ifndef MM__PMM_H__
#define MM__PMM_H__

#include <stdint.h>
#include <stddef.h>

void *ext_mem_alloc(uint64_t count);
void pmm_free(void *ptr, uint64_t length);

#endif
