#ifndef LIB__FDT_H
#define LIB__FDT_H

#if !defined(__x86_64__) && !defined(__i386__)

#include <stddef.h>
#include <stdint.h>

int fdt_set_chosen_string(void *fdt, const char *name, const char *value);
int fdt_set_chosen_uint64(void *fdt, const char *name, uint64_t value);
int fdt_set_chosen_uint32(void *fdt, const char *name, uint32_t value);

#endif

#endif
