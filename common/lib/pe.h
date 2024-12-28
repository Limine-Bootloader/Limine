#ifndef LIB__PE_H__
#define LIB__PE_H__

#include <stdint.h>
#include <stdbool.h>
#include <lib/misc.h>

int pe_bits(uint8_t *image);

bool pe64_load(uint8_t *image, uint64_t *entry_point, uint64_t *_slide, uint32_t alloc_type, bool kaslr, struct mem_range **ranges, uint64_t *ranges_count, uint64_t *physical_base, uint64_t *virtual_base, uint64_t *image_size, uint64_t *image_size_before_bss, bool *is_reloc);

#endif
