#if defined (BIOS)

#include <stdint.h>
#include <stddef.h>
#include <sys/e820.h>
#include <lib/real.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <mm/pmm.h>

#define MAX_E820_ENTRIES 256

struct memmap_entry e820_map[MAX_E820_ENTRIES];
size_t e820_entries = 0;

void init_e820(void) {
    struct rm_regs r = {0};

    // A BIOS is known to assume the buffer carries over between calls and to
    // update only the fields it changes, so it is zeroed once, not per call.
    struct memmap_entry entry = {0};

    for (size_t i = 0; i < MAX_E820_ENTRIES; i++) {
        r.eax = 0xe820;
        r.ecx = 24;
        r.edx = 0x534d4150;
        r.edi = (uint32_t)&entry;
        rm_int(0x15, &r, &r);

        if (r.eflags & EFLAGS_CF) {
            e820_entries = i;
            return;
        }

        e820_map[i] = entry;

        if (!r.ebx) {
            e820_entries = ++i;
            return;
        }
    }

    panic(false, "Too many E820 entries!");
}

#endif
