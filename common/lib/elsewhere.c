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
        bool flexible_target,
        struct elsewhere_range *ranges, uint64_t *ranges_count,
        uint64_t ranges_max,
        void *elsewhere, uint64_t *target, size_t t_length) {
    // original target of -1 means "allocate after top of all ranges"
    // flexible target is ignored
    flexible_target = true;
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
            return false;
        }

        // Does it overlap with other elsewhere ranges targets?
        {
            uint64_t base = ranges[i].target;
            uint64_t length = ranges[i].length;
            uint64_t top = CHECKED_ADD(base, length, continue);

            if (elsewhere_overlap_check(base, top, *target, t_top)) {
                if (!flexible_target) {
                    return false;
                }
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
                if (!flexible_target) {
                    return false;
                }
                *target = ALIGN_UP(top, 4096, return false);
                goto retry;
            }
        }

        // Make sure it is memory that actually exists.
        if (!memmap_alloc_range(*target, t_length, MEMMAP_BOOTLOADER_RECLAIMABLE,
                                MEMMAP_USABLE, false, true, false)) {
            if (!memmap_alloc_range(*target, t_length, MEMMAP_BOOTLOADER_RECLAIMABLE,
                                    MEMMAP_BOOTLOADER_RECLAIMABLE, false, true, false)) {
                if (!flexible_target) {
                    return false;
                }
                *target += 0x1000;
                goto retry;
            }
        }
    }

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
