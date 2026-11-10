#ifndef PROTOS__MULTIBOOT_H
#define PROTOS__MULTIBOOT_H

#include <stdint.h>
#include <lib/misc.h>

struct mb_reloc_stub {
    char jmp[4];
    uint32_t magic;
    uint32_t entry_point;
    uint32_t mb_info_target;
};

extern symbol multiboot_spinup_32;
extern symbol multiboot_reloc_stub, multiboot_reloc_stub_end;

#endif
