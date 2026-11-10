#if defined(BIOS)

#include <stdint.h>
#include <stddef.h>
#include <sys/e820.h>
#include <lib/real.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <mm/pmm.h>

struct memmap_entry e820_map[MAX_E820_ENTRIES];
size_t e820_entries = 0;

void init_e820(void) {
    // A BIOS is known to assume the buffer carries over between calls and to
    // update only the fields it changes, so it is zeroed once, not per call.
    struct memmap_entry entry = {0};
    uint32_t continuation = 0;

    for (size_t i = 0; i < MAX_E820_ENTRIES; i++) {
        // Only the continuation value carries over: the other inputs are set
        // afresh, in case a BIOS changes one it is meant to keep.
        struct rm_regs r = {0};
        r.eax = 0xe820;
        r.ebx = continuation;
        r.ecx = 24;
        r.edx = 0x534d4150;
        r.edi = (uint32_t)&entry;
        rm_int(0x15, &r, &r);

        if (r.eflags & EFLAGS_CF) {
            e820_entries = i;
            return;
        }

        // A BIOS that stops echoing the signature mid-map leaves no way to
        // tell a partial map from a complete one.
        if (r.eax != 0x534d4150) {
            panic(false, "E820 signature mismatch, memory map is unreliable");
        }

        e820_map[i] = entry;

        if (!r.ebx) {
            e820_entries = ++i;
            return;
        }

        continuation = r.ebx;
    }

    panic(false, "Too many E820 entries!");
}

#endif
