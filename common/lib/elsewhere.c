#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/elsewhere.h>
#include <lib/misc.h>
#include <mm/pmm.h>

static bool elsewhere_overlap_check(uint64_t base1, uint64_t top1,
                              uint64_t base2, uint64_t top2) {
    return (base1 < top2 && base2 < top1);
}

bool elsewhere_append(
        bool allow_wraparound,
        struct elsewhere_range *ranges, uint64_t *ranges_count,
        uint64_t ranges_max,
        void *elsewhere, uint64_t *target, size_t t_length) {
    // A target of -1 means "allocate after the top of all ranges".
    bool wrapped = false;
    if (*target == (uint64_t)-1) {
        uint64_t top = 0;

        for (size_t i = 0; i < *ranges_count; i++) {
            uint64_t r_top = CHECKED_ADD(ranges[i].target, ranges[i].length, continue);

            if (top < r_top) {
                top = r_top;
            }
        }

        *target = ALIGN_UP(top, 4096, return false);
    }

    uint64_t max_retries = 0x10000;

retry:
    if (max_retries-- == 0) {
        return false;
    }

    for (size_t i = 0; i < *ranges_count; i++) {
        uint64_t t_top = CHECKED_ADD(*target, t_length, return false);

        // Ensure allocation stays within 32-bit address space.
        if (t_top > 0x100000000) {
            // If permitted, wrap the search around to low memory once. This lets
            // targets be placed below a kernel relocated high, which would
            // otherwise leave no room above it for the info struct or modules.
            if (allow_wraparound && !wrapped) {
                wrapped = true;
                *target = 0x100000;
                goto retry;
            }
            return false;
        }

        // Does it overlap with other elsewhere ranges targets?
        {
            uint64_t base = ranges[i].target;
            uint64_t length = ranges[i].length;
            uint64_t top = CHECKED_ADD(base, length, continue);

            if (elsewhere_overlap_check(base, top, *target, t_top)) {
                *target = ALIGN_UP(top, 4096, return false);
                goto retry;
            }
        }

        // Does it overlap with other elsewhere ranges sources?
        {
            uint64_t base = ranges[i].elsewhere;
            uint64_t length = ranges[i].length;
            uint64_t top = CHECKED_ADD(base, length, continue);

            if (elsewhere_overlap_check(base, top, *target, t_top)) {
                *target = ALIGN_UP(top, 4096, return false);
                goto retry;
            }
        }

        // Make sure it is memory that actually exists.
        if (!memmap_alloc_range(*target, t_length, MEMMAP_BOOTLOADER_RECLAIMABLE,
                                MEMMAP_USABLE, false, true, false)) {
            if (!memmap_alloc_range(*target, t_length, MEMMAP_BOOTLOADER_RECLAIMABLE,
                                    MEMMAP_BOOTLOADER_RECLAIMABLE, false, true, false)) {
                *target += 0x1000;
                goto retry;
            }
        }
    }

    // Reserve the chosen target so later sources can't be placed over it.
    elsewhere_reserve_target(*target, t_length);

    // Add the elsewhere range
    if (*ranges_count >= ranges_max) {
        panic(false, "elsewhere: ranges array overflow");
    }
    ranges[*ranges_count].elsewhere = (uintptr_t)elsewhere;
    ranges[*ranges_count].target = *target;
    ranges[*ranges_count].length = t_length;
    *ranges_count += 1;

    return true;
}

// Reserve the USABLE portions of [base, base+length) as bootloader-reclaimable
// so a later ext_mem_alloc() source can't be placed on top of an already
// committed target. Portions that are already non-USABLE (a target sitting on
// its own staging source) are left alone; ext_mem_alloc() avoids those anyway.
void elsewhere_reserve_target(uint64_t base, uint64_t length) {
    uint64_t top = CHECKED_ADD(base, length, return);

    // Snapshot the usable sub-ranges first; memmap_alloc_range() mutates the
    // map, which would invalidate a live iteration.
    struct { uint64_t base, length; } parts[64];
    size_t n = 0;
    for (size_t i = 0; i < memmap_entries && n < 64; i++) {
        if (memmap[i].type != MEMMAP_USABLE) {
            continue;
        }
        uint64_t e_base = memmap[i].base;
        uint64_t e_top = CHECKED_ADD(e_base, memmap[i].length, continue);
        uint64_t o_base = base > e_base ? base : e_base;
        uint64_t o_top = top < e_top ? top : e_top;
        if (o_base < o_top) {
            parts[n].base = o_base;
            parts[n].length = o_top - o_base;
            n++;
        }
    }

    for (size_t i = 0; i < n; i++) {
        memmap_alloc_range(parts[i].base, parts[i].length,
                           MEMMAP_BOOTLOADER_RECLAIMABLE, MEMMAP_USABLE,
                           false, false, false);
    }
}
