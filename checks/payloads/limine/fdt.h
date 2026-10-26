#ifndef FDT_H
#define FDT_H

#include <stdint.h>
#include <stdbool.h>

bool fdt_magic_ok(const void *dtb);
uint32_t fdt_totalsize(const void *dtb);
// Nodes directly under the root named exactly name, or name@unit.
int fdt_count_root_nodes(const void *dtb, const char *name);
// A property of the node at path ("/" or "/a/b"), or NULL.
const void *fdt_getprop_path(const void *dtb, const char *path, const char *prop, int *len);

static inline uint32_t be32(const void *p) {
    const uint8_t *b = p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static inline uint64_t be64(const void *p) {
    return ((uint64_t)be32(p) << 32) | be32((const uint8_t *)p + 4);
}

#endif
