#ifndef LIB__SMBIOS_H
#define LIB__SMBIOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct smbios_struct_header {
    uint8_t type;
    uint8_t length;
    uint16_t handle;
} __attribute__((packed));

struct smbios_walk {
    uintptr_t table;
    size_t length;
    size_t offset;
    // What the 32-bit entry point counts, 0 where there is no count to go by.
    size_t count;
    size_t index;
};

// The table of the 64-bit entry point where it is within reach, else that of
// the 32-bit one. False where there is none.
bool smbios_walk_start(struct smbios_walk *walk);

// The next structure and, in *size, its size with its strings, or NULL once
// the table ends or holds a structure that does not fit in it.
struct smbios_struct_header *smbios_walk_next(struct smbios_walk *walk, size_t *size);

#endif
