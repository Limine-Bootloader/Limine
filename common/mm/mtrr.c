#if defined (__x86_64__) || defined (__i386__)

#include <stdint.h>
#include <stddef.h>
#include <mm/mtrr.h>
#include <mm/pmm.h>
#include <sys/cpu.h>
#include <lib/print.h>
#include <lib/misc.h>

static bool mtrr_supported(void) {
    uint32_t eax, ebx, ecx, edx;

    if (!cpuid(1, 0, &eax, &ebx, &ecx, &edx))
        return false;

    return !!(edx & (1 << 12));
}

uint64_t *saved_mtrrs = NULL;

void mtrr_save(void) {
    if (!mtrr_supported()) {
        return;
    }

    uint64_t ia32_mtrrcap = rdmsr(0xfe);

    uint8_t var_reg_count = ia32_mtrrcap & 0xff;
    bool fix_supported = !!(ia32_mtrrcap & ((uint64_t)1 << 8));

    if (saved_mtrrs == NULL) {
        saved_mtrrs = ext_mem_alloc((
            (var_reg_count * 2) /* variable MTRRs, 2 MSRs each */
          + 11                  /* 11 fixed MTRRs */
          + 1                   /* 1 default type MTRR */
        ) * sizeof(uint64_t));
    }

    /* save variable range MTRRs */
    for (uint8_t i = 0; i < var_reg_count * 2; i += 2) {
        saved_mtrrs[i] = rdmsr(0x200 + i);
        saved_mtrrs[i + 1] = rdmsr(0x200 + i + 1);
    }

    /* save fixed range MTRRs, if supported by the CPU */
    if (fix_supported) {
        saved_mtrrs[var_reg_count * 2 + 0] = rdmsr(0x250);
        saved_mtrrs[var_reg_count * 2 + 1] = rdmsr(0x258);
        saved_mtrrs[var_reg_count * 2 + 2] = rdmsr(0x259);
        saved_mtrrs[var_reg_count * 2 + 3] = rdmsr(0x268);
        saved_mtrrs[var_reg_count * 2 + 4] = rdmsr(0x269);
        saved_mtrrs[var_reg_count * 2 + 5] = rdmsr(0x26a);
        saved_mtrrs[var_reg_count * 2 + 6] = rdmsr(0x26b);
        saved_mtrrs[var_reg_count * 2 + 7] = rdmsr(0x26c);
        saved_mtrrs[var_reg_count * 2 + 8] = rdmsr(0x26d);
        saved_mtrrs[var_reg_count * 2 + 9] = rdmsr(0x26e);
        saved_mtrrs[var_reg_count * 2 + 10] = rdmsr(0x26f);
    }

    /* save MTRR default type */
    saved_mtrrs[var_reg_count * 2 + 11] = rdmsr(0x2ff);

    /* make sure that the saved MTRR default has MTRRs off */
    saved_mtrrs[var_reg_count * 2 + 11] &= ~((uint64_t)1 << 11);
}

void mtrr_restore(void) {
    if (!mtrr_supported()) {
        return;
    }

    uint64_t ia32_mtrrcap = rdmsr(0xfe);

    uint8_t var_reg_count = ia32_mtrrcap & 0xff;
    bool fix_supported = !!(ia32_mtrrcap & ((uint64_t)1 << 8));

    if (saved_mtrrs == NULL) {
        panic(true, "mtrr: Attempted restore without prior save");
    }

    /* according to the Intel SDM 12.11.7.2 "MemTypeSet() Function",
       we need to follow this procedure before changing MTRR set up */

    /* save old cr0 and then enable the CD flag and disable the NW flag */
    uintptr_t old_cr0;
    asm volatile ("mov %%cr0, %0" : "=r"(old_cr0) :: "memory");
    uintptr_t new_cr0 = (old_cr0 | (1 << 30)) & ~((uintptr_t)1 << 29);
    asm volatile ("mov %0, %%cr0" :: "r"(new_cr0) : "memory");

    /* then invalidate the caches */
    asm volatile ("wbinvd" ::: "memory");

    /* do a cr3 read/write to flush the TLB */
    uintptr_t cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3) :: "memory");
    asm volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");

    /* disable the MTRRs */
    uint64_t mtrr_def = rdmsr(0x2ff);
    mtrr_def &= ~((uint64_t)1 << 11);
    wrmsr(0x2ff, mtrr_def);

    /* restore variable range MTRRs */
    for (uint8_t i = 0; i < var_reg_count * 2; i += 2) {
        wrmsr(0x200 + i, saved_mtrrs[i]);
        wrmsr(0x200 + i + 1, saved_mtrrs[i + 1]);
    }

    /* restore fixed range MTRRs, if supported by the CPU */
    if (fix_supported) {
        wrmsr(0x250, saved_mtrrs[var_reg_count * 2 + 0]);
        wrmsr(0x258, saved_mtrrs[var_reg_count * 2 + 1]);
        wrmsr(0x259, saved_mtrrs[var_reg_count * 2 + 2]);
        wrmsr(0x268, saved_mtrrs[var_reg_count * 2 + 3]);
        wrmsr(0x269, saved_mtrrs[var_reg_count * 2 + 4]);
        wrmsr(0x26a, saved_mtrrs[var_reg_count * 2 + 5]);
        wrmsr(0x26b, saved_mtrrs[var_reg_count * 2 + 6]);
        wrmsr(0x26c, saved_mtrrs[var_reg_count * 2 + 7]);
        wrmsr(0x26d, saved_mtrrs[var_reg_count * 2 + 8]);
        wrmsr(0x26e, saved_mtrrs[var_reg_count * 2 + 9]);
        wrmsr(0x26f, saved_mtrrs[var_reg_count * 2 + 10]);
    }

    /* restore MTRR default type */
    wrmsr(0x2ff, saved_mtrrs[var_reg_count * 2 + 11]);

    /* now do the opposite of the cache disable and flush from above */

    /* re-enable MTRRs */
    mtrr_def = rdmsr(0x2ff);
    mtrr_def |= (1 << 11);
    wrmsr(0x2ff, mtrr_def);

    /* do a cr3 read/write to flush the TLB */
    asm volatile ("mov %%cr3, %0" : "=r"(cr3) :: "memory");
    asm volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");

    /* then invalidate the caches */
    asm volatile ("wbinvd" ::: "memory");

    /* restore old value of cr0 */
    asm volatile ("mov %0, %%cr0" :: "r"(old_cr0) : "memory");
}

#define MTRR_TYPE_WC 1
#define WC_MAX_SLOTS 4

static struct {
    uint8_t  slot;
    uint64_t saved_base;
    uint64_t saved_mask;
} wc_saved[WC_MAX_SLOTS];
static size_t wc_n_saved = 0;

bool mtrr_wc_add_fb_range(uint64_t base, uint64_t size) {
    if (size == 0 || !mtrr_supported()) {
        return false;
    }
    if (wc_n_saved >= WC_MAX_SLOTS) {
        return false;
    }

    uint64_t mtrrcap = rdmsr(0xfe);
    if (!(mtrrcap & ((uint64_t)1 << 10))) {
        return false;
    }
    uint8_t var_reg_count = mtrrcap & 0xff;

    uint32_t eax, ebx, ecx, edx;
    if (!cpuid(0x80000008, 0, &eax, &ebx, &ecx, &edx)) {
        return false;
    }
    uint8_t maxphysaddr = eax & 0xff;
    if (maxphysaddr < 32 || maxphysaddr > 52) {
        return false;
    }

    base &= ~(uint64_t)0xfff;

    uint64_t aligned_size = 0x1000;
    while (aligned_size < size) {
        aligned_size <<= 1;
        if (aligned_size == 0) {
            return false;
        }
    }
    size = aligned_size;

    // MTRR match is (addr & mask) == (base & mask); only correct when base is size-aligned.
    if (base & (size - 1)) {
        return false;
    }

    uint64_t mask = (((uint64_t)1 << maxphysaddr) - 1) & ~(size - 1);

    for (uint8_t i = 0; i < var_reg_count; i++) {
        uint64_t mb = rdmsr(0x200 + i * 2);
        uint64_t mm = rdmsr(0x200 + i * 2 + 1);
        if (!(mm & ((uint64_t)1 << 11))) {
            continue;
        }
        uint64_t exist_mask = mm & ~(uint64_t)0xfff;
        uint64_t exist_base = mb & ~(uint64_t)0xfff;
        for (uint64_t a = base; a < base + size; a += 0x1000) {
            if ((a & exist_mask) == (exist_base & exist_mask)) {
                return false;
            }
        }
    }

    uint8_t slot;
    bool found = false;
    for (uint8_t i = 0; i < var_reg_count; i++) {
        uint64_t mm = rdmsr(0x200 + i * 2 + 1);
        if (!(mm & ((uint64_t)1 << 11))) {
            slot = i;
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }

#if defined (UEFI)
    asm volatile ("cli");
#endif

    uintptr_t old_cr0;
    asm volatile ("mov %%cr0, %0" : "=r"(old_cr0) :: "memory");
    uintptr_t new_cr0 = (old_cr0 | (1U << 30)) & ~((uintptr_t)1 << 29);
    asm volatile ("mov %0, %%cr0" :: "r"(new_cr0) : "memory");
    asm volatile ("wbinvd" ::: "memory");

    uintptr_t cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3) :: "memory");
    asm volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");

    uint64_t mtrr_def = rdmsr(0x2ff);
    wrmsr(0x2ff, mtrr_def & ~((uint64_t)1 << 11));

    wc_saved[wc_n_saved].slot       = slot;
    wc_saved[wc_n_saved].saved_base = rdmsr(0x200 + slot * 2);
    wc_saved[wc_n_saved].saved_mask = rdmsr(0x200 + slot * 2 + 1);
    wc_n_saved++;

    wrmsr(0x200 + slot * 2,     base | MTRR_TYPE_WC);
    wrmsr(0x200 + slot * 2 + 1, mask | ((uint64_t)1 << 11));

    wrmsr(0x2ff, mtrr_def | ((uint64_t)1 << 11));

    asm volatile ("mov %%cr3, %0" : "=r"(cr3) :: "memory");
    asm volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
    asm volatile ("wbinvd" ::: "memory");
    asm volatile ("mov %0, %%cr0" :: "r"(old_cr0) : "memory");

#if defined (UEFI)
    asm volatile ("sti");
#endif

    return true;
}

void mtrr_wc_clear_fb_ranges(void) {
    if (wc_n_saved == 0) {
        return;
    }
    if (!mtrr_supported()) {
        wc_n_saved = 0;
        return;
    }

#if defined (UEFI)
    asm volatile ("cli");
#endif

    uintptr_t old_cr0;
    asm volatile ("mov %%cr0, %0" : "=r"(old_cr0) :: "memory");
    uintptr_t new_cr0 = (old_cr0 | (1U << 30)) & ~((uintptr_t)1 << 29);
    asm volatile ("mov %0, %%cr0" :: "r"(new_cr0) : "memory");
    asm volatile ("wbinvd" ::: "memory");

    uintptr_t cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3) :: "memory");
    asm volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");

    uint64_t mtrr_def = rdmsr(0x2ff);
    wrmsr(0x2ff, mtrr_def & ~((uint64_t)1 << 11));

    for (size_t i = 0; i < wc_n_saved; i++) {
        wrmsr(0x200 + wc_saved[i].slot * 2,     wc_saved[i].saved_base);
        wrmsr(0x200 + wc_saved[i].slot * 2 + 1, wc_saved[i].saved_mask);
    }

    wrmsr(0x2ff, mtrr_def);

    asm volatile ("mov %%cr3, %0" : "=r"(cr3) :: "memory");
    asm volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
    asm volatile ("wbinvd" ::: "memory");
    asm volatile ("mov %0, %%cr0" :: "r"(old_cr0) : "memory");

#if defined (UEFI)
    asm volatile ("sti");
#endif

    wc_n_saved = 0;
}

#endif
