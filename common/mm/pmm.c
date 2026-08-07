#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <mm/pmm.h>
#include <lib/rand.h>
#include <lib/print.h>
#include <lib/misc.h>

static bool full_overlap_check(uint64_t base1, uint64_t top1,
                               uint64_t base2, uint64_t top2) {
    return ((base1 >= base2 && base1 <  top2)
         && (top1  >  base2 && top1  <= top2));
}

bool check_usable_memory(uint64_t base, uint64_t top) {
    uint64_t overlap_remaining = top - base;

    for (size_t i = 0; i < memmap_entries; i++) {
        if (memmap[i].type != MEMMAP_USABLE
#if defined (UEFI)
         && memmap[i].type != MEMMAP_EFI_RECLAIMABLE
#endif
         && memmap[i].type != MEMMAP_BOOTLOADER_RECLAIMABLE
         && memmap[i].type != MEMMAP_KERNEL_AND_MODULES) {
            continue;
        }

        uint64_t memmap_top = CHECKED_ADD(memmap[i].base, memmap[i].length, continue);

        if (full_overlap_check(base, top, memmap[i].base, memmap_top)) {
            return true;
        }

        // Count how many bytes from a real-RAM entry overlap our range
        if (memmap[i].base < top && memmap_top > base) {
            uint64_t overlap_bottom = memmap[i].base > base ? memmap[i].base : base;
            uint64_t overlap_top = memmap_top < top ? memmap_top : top;

            uint64_t overlap_size = overlap_top - overlap_bottom;
            overlap_remaining -= overlap_size;

            if (overlap_remaining == 0) {
                return true;
            }
        }
    }

    return false;
}

void pmm_randomise_memory(void) {
    print("pmm: Randomising memory contents...");

    for (size_t i = 0; i < memmap_entries; i++) {
        if (memmap[i].type != MEMMAP_USABLE)
            continue;

#if defined (__i386__)
        // A 32-bit build cannot address it, and the cast below truncates
        // rather than failing.
        if (memmap[i].base >= 0x100000000) {
            continue;
        }
#endif

        uint8_t *ptr = (void *)(uintptr_t)memmap[i].base;
        size_t len = memmap[i].length;

        for (size_t j = 0;;) {
            uint32_t random = rand32();
            uint8_t *rnd_data = (void *)&random;
            if (j >= len)
                break;
            ptr[j++] = rnd_data[0];
            if (j >= len)
                break;
            ptr[j++] = rnd_data[1];
            if (j >= len)
                break;
            ptr[j++] = rnd_data[2];
            if (j >= len)
                break;
            ptr[j++] = rnd_data[3];
        }
    }

    print("\n");
}
