#if defined (__x86_64__) && defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <mm/efi_pt.h>
#include <sys/cpu.h>

#define PTE_P ((uint64_t)1 << 0)
#define PTE_PWT ((uint64_t)1 << 3)
#define PTE_PCD ((uint64_t)1 << 4)
#define PTE_PS ((uint64_t)1 << 7)
#define PTE_PAT_4K ((uint64_t)1 << 7)
#define PTE_PAT_BIG ((uint64_t)1 << 12)
#define PT_ADDR_MASK ((uint64_t)0x000FFFFFFFFFF000)

#define UCM_MASK_4K (PTE_PWT | PTE_PCD | PTE_PAT_4K)
#define UCM_MASK_BIG (PTE_PWT | PTE_PCD | PTE_PAT_BIG)
#define UCM_VALUE PTE_PCD

#define IA32_PAT_MSR 0x277
#define PAT_TYPE_UCM 0x07

static bool la57_enabled(void) {
    uint64_t cr4;
    asm volatile ("mov %%cr4, %0" : "=r"(cr4));
    return !!(cr4 & ((uint64_t)1 << 12));
}

static bool pat_slot2_is_ucm(void) {
    static bool checked = false, ok = false;
    if (checked) {
        return ok;
    }
    checked = true;

    uint32_t eax, ebx, ecx, edx;
    if (!cpuid(1, 0, &eax, &ebx, &ecx, &edx) || !(edx & (1 << 16))) {
        return false;
    }

    ok = ((rdmsr(IA32_PAT_MSR) >> 16) & 0xff) == PAT_TYPE_UCM;
    return ok;
}

static uint64_t *walk_to_leaf(uint64_t addr, uint64_t *pg_size, bool *large) {
    uint64_t cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3));

    uint64_t *table = (uint64_t *)(cr3 & PT_ADDR_MASK);

    int lvl = la57_enabled() ? 5 : 4;

    for (; lvl >= 1; lvl--) {
        int shift = (lvl - 1) * 9 + 12;
        size_t idx = (addr >> shift) & 0x1ff;
        uint64_t e = table[idx];

        if (!(e & PTE_P)) {
            return NULL;
        }

        bool is_leaf = (lvl == 1) || ((lvl == 2 || lvl == 3) && (e & PTE_PS));
        if (is_leaf) {
            *pg_size = (uint64_t)1 << shift;
            *large = (lvl != 1);
            return &table[idx];
        }

        table = (uint64_t *)(e & PT_ADDR_MASK);
    }
    return NULL;
}

void efi_pt_set_fb_uc_minus(uint64_t base, uint64_t size) {
    if (size == 0 || !pat_slot2_is_ucm()) {
        return;
    }

    // Clear CR0.WP so supervisor writes hit any firmware PTEs mapped R/O.
    uint64_t old_cr0;
    asm volatile ("mov %%cr0, %0" : "=r"(old_cr0));
    asm volatile ("mov %0, %%cr0" :: "r"(old_cr0 & ~((uint64_t)1 << 16)) : "memory");

    uint64_t end = (base + size + 0xfff) & ~(uint64_t)0xfff;
    base &= ~(uint64_t)0xfff;

    while (base < end) {
        uint64_t pg;
        bool large;
        uint64_t *entry = walk_to_leaf(base, &pg, &large);
        if (entry == NULL) {
            base += 0x1000;
            continue;
        }

        uint64_t mask = large ? UCM_MASK_BIG : UCM_MASK_4K;
        *entry = (*entry & ~mask) | UCM_VALUE;

        invlpg(base);
        base = (base & ~(pg - 1)) + pg;
    }

    asm volatile ("mov %0, %%cr0" :: "r"(old_cr0) : "memory");
}

#endif
