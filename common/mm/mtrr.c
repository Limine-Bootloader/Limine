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

#endif
