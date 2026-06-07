#if defined (__x86_64__) && defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <mm/efi_pt.h>
#include <mm/pmm.h>
#include <sys/cpu.h>
#include <lib/misc.h>

#define PTE_P ((uint64_t)1 << 0)
#define PTE_RW ((uint64_t)1 << 1)
#define PTE_US ((uint64_t)1 << 2)
#define PTE_PWT ((uint64_t)1 << 3)
#define PTE_PCD ((uint64_t)1 << 4)
#define PTE_PS ((uint64_t)1 << 7)
#define PTE_PAT_4K ((uint64_t)1 << 7)
#define PTE_PAT_BIG ((uint64_t)1 << 12)
#define PTE_NX ((uint64_t)1 << 63)
#define PT_ADDR_MASK ((uint64_t)0x000FFFFFFFFFF000)

#define IA32_PAT_MSR 0x277
#define PAT_TYPE_WC 0x01

static bool la57_enabled(void) {
    uint64_t cr4;
    asm volatile ("mov %%cr4, %0" : "=r"(cr4));
    return !!(cr4 & ((uint64_t)1 << 12));
}

static bool gib_pages_supported(void) {
    uint32_t eax, ebx, ecx, edx;
    return cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx) && !!(edx & (1 << 26));
}

// Firmware PTEs we overwrote, replayed by efi_pt_restore() to undo the FB WC.
// On overflow we stop applying WC so the undo stays complete.
#define SAVED_PTES_MAX 1024

static uint64_t **saved_pte_ptr = NULL;
static uint64_t *saved_pte_val = NULL;
static size_t saved_pte_i = 0;
static uint64_t saved_pat = 0;
static bool pat_modified = false;

static int wc_pat_index = -1;
static bool gib_supported = false;

static bool save_pte(uint64_t *slot) {
    if (saved_pte_i >= SAVED_PTES_MAX) {
        return false;
    }
    saved_pte_ptr[saved_pte_i] = slot;
    saved_pte_val[saved_pte_i] = *slot;
    saved_pte_i++;
    return true;
}

// Caches off + TLB flushed for a safe PAT-MSR memory-type change.
static void cache_off(uint64_t *old_cr0) {
    asm volatile ("mov %%cr0, %0" : "=r"(*old_cr0) :: "memory");
    asm volatile ("mov %0, %%cr0"
        :: "r"((*old_cr0 | ((uint64_t)1 << 30)) & ~((uint64_t)1 << 29))
        : "memory");
    asm volatile ("wbinvd" ::: "memory");
    uint64_t cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3) :: "memory");
    asm volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

static void cache_on(uint64_t old_cr0) {
    uint64_t cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3) :: "memory");
    asm volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
    asm volatile ("wbinvd" ::: "memory");
    asm volatile ("mov %0, %%cr0" :: "r"(old_cr0) : "memory");
}

// WP off to write read-only firmware PTEs; wp_on flushes the TLB to apply them.
static void wp_off(uint64_t *old_cr0) {
    asm volatile ("mov %%cr0, %0" : "=r"(*old_cr0) :: "memory");
    asm volatile ("mov %0, %%cr0" :: "r"(*old_cr0 & ~((uint64_t)1 << 16)) : "memory");
}

static void wp_on(uint64_t old_cr0) {
    // A plain CR3 reload leaves global TLB entries intact, so a firmware FB
    // mapped with the global bit would keep its old memory type. Toggling
    // CR4.PGE flushes the whole TLB including global entries (Intel SDM
    // 4.10.4.1); fall back to a CR3 reload when PGE is off (no globals exist).
    uint64_t cr4;
    asm volatile ("mov %%cr4, %0" : "=r"(cr4) :: "memory");
    if (cr4 & ((uint64_t)1 << 7)) {
        asm volatile ("mov %0, %%cr4" :: "r"(cr4 & ~((uint64_t)1 << 7)) : "memory");
        asm volatile ("mov %0, %%cr4" :: "r"(cr4) : "memory");
    } else {
        uint64_t cr3;
        asm volatile ("mov %%cr3, %0" : "=r"(cr3) :: "memory");
        asm volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
    }
    asm volatile ("mov %0, %%cr0" :: "r"(old_cr0) : "memory");
}

static int pte_pat_index(uint64_t e, bool leaf4k) {
    uint64_t patbit = leaf4k ? PTE_PAT_4K : PTE_PAT_BIG;
    return (!!(e & patbit) << 2) | (!!(e & PTE_PCD) << 1) | !!(e & PTE_PWT);
}

// Non-leaf entries and CR3 have no PAT bit: typed by PCD/PWT only.
static int walk_pat_index(uint64_t e) {
    return (!!(e & PTE_PCD) << 1) | !!(e & PTE_PWT);
}

static void scan_walk(uint64_t *table, int lvl, uint8_t *used) {
    for (size_t i = 0; i < 512; i++) {
        if (*used == 0xff) {
            return;
        }
        uint64_t e = table[i];
        if (!(e & PTE_P)) {
            continue;
        }
        if ((lvl == 1) || (lvl <= 3 && (e & PTE_PS))) {
            *used |= 1 << pte_pat_index(e, lvl == 1);
        } else {
            *used |= 1 << walk_pat_index(e);
            scan_walk((uint64_t *)(e & PT_ADDR_MASK), lvl - 1, used);
        }
    }
}

static uint8_t scan_used_pat_indices(void) {
    uint64_t cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3));
    uint8_t used = 1 << walk_pat_index(cr3);
    scan_walk((uint64_t *)(cr3 & PT_ADDR_MASK), la57_enabled() ? 5 : 4, &used);
    return used;
}

// Reuse a WC slot if present, else repurpose one no live mapping selects.
static bool ensure_wc_pat_slot(void) {
    if (wc_pat_index >= 0) {
        return true;
    }

    uint32_t eax, ebx, ecx, edx;
    if (!cpuid(1, 0, &eax, &ebx, &ecx, &edx) || !(edx & (1 << 16))) {
        return false;
    }

    uint64_t pat = rdmsr(IA32_PAT_MSR);

    for (int i = 0; i < 8; i++) {
        if (((pat >> (i * 8)) & 0xff) == PAT_TYPE_WC) {
            wc_pat_index = i;
            return true;
        }
    }

    uint8_t used = scan_used_pat_indices();

    int slot = -1;
    for (int i = 4; i < 8; i++) {
        if (!(used & (1 << i))) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < 4; i++) {
            if (!(used & (1 << i))) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        return false;
    }

    saved_pat = pat;
    pat_modified = true;

    pat = (pat & ~((uint64_t)0xff << (slot * 8)))
        | ((uint64_t)PAT_TYPE_WC << (slot * 8));

    uint64_t old_cr0;
    cache_off(&old_cr0);
    wrmsr(IA32_PAT_MSR, pat);
    cache_on(old_cr0);

    wc_pat_index = slot;
    return true;
}

static uint64_t pte_set_pat(uint64_t e, int idx, bool leaf4k) {
    uint64_t patbit = leaf4k ? PTE_PAT_4K : PTE_PAT_BIG;
    e &= ~(PTE_PWT | PTE_PCD | patbit);
    if (idx & 1) e |= PTE_PWT;
    if (idx & 2) e |= PTE_PCD;
    if (idx & 4) e |= patbit;
    return e;
}

// Children inherit the parent mapping so memory sharing the leaf keeps its type.
static uint64_t *split_leaf(uint64_t e, int lvl) {
    uint64_t leaf_sz = (uint64_t)1 << ((lvl - 1) * 9 + 12);
    uint64_t phys_base = e & PT_ADDR_MASK & ~(leaf_sz - 1);
    int idx = pte_pat_index(e, false);
    uint64_t flags = e & (PTE_P | PTE_RW | PTE_US | PTE_NX);

    int child_lvl = lvl - 1;
    uint64_t child_sz = (uint64_t)1 << ((child_lvl - 1) * 9 + 12);
    bool child_4k = child_lvl == 1;

    uint64_t *t = ext_mem_alloc(0x1000);
    for (int i = 0; i < 512; i++) {
        uint64_t c = (phys_base + (uint64_t)i * child_sz) | flags;
        if (!child_4k) {
            c |= PTE_PS;
        }
        t[i] = pte_set_pat(c, idx, child_4k);
    }
    return t;
}

// pristine: inside firmware tables (saved); tables we allocate are not.
static void wc_walk(uint64_t *table, int lvl, uint64_t tbl_va,
                    uint64_t base, uint64_t end, bool pristine) {
    uint64_t step = (uint64_t)1 << ((lvl - 1) * 9 + 12);

    for (size_t i = 0; i < 512; i++) {
        uint64_t va = tbl_va + (uint64_t)i * step;
        if (va >= end) {
            break;
        }
        if (va + step <= base) {
            continue;
        }

        uint64_t *e = &table[i];
        bool present = !!(*e & PTE_P);
        bool is_leaf = (lvl == 1) || (present && lvl <= 3 && (*e & PTE_PS));
        bool fully = va >= base && va + step <= end;

        if (fully && lvl <= 3 && (lvl < 3 || gib_supported)) {
            uint64_t leaf;
            if (present && is_leaf) {
                leaf = (*e & PT_ADDR_MASK & ~(step - 1))
                     | (*e & (PTE_P | PTE_RW | PTE_US | PTE_NX));
            } else {
                leaf = va | PTE_P | PTE_RW;
            }
            if (lvl >= 2) {
                leaf |= PTE_PS;
            }
            if (pristine && !save_pte(e)) {
                continue;
            }
            *e = pte_set_pat(leaf, wc_pat_index, lvl == 1);
            continue;
        }

        if (present && !is_leaf) {
            wc_walk((uint64_t *)(*e & PT_ADDR_MASK), lvl - 1, va,
                    base, end, pristine);
            continue;
        }

        if (pristine && !save_pte(e)) {
            continue;
        }
        uint64_t *child;
        if (present) {
            child = split_leaf(*e, lvl);
        } else {
            child = ext_mem_alloc(0x1000);
        }
        *e = (uint64_t)child | PTE_P | PTE_RW | PTE_US;
        wc_walk(child, lvl - 1, va, base, end, false);
    }
}

void efi_pt_set_fb_wc(uint64_t base, uint64_t size) {
    if (size == 0) {
        return;
    }

    if (saved_pte_ptr == NULL) {
        saved_pte_ptr = ext_mem_alloc(SAVED_PTES_MAX * sizeof(uint64_t *));
        saved_pte_val = ext_mem_alloc(SAVED_PTES_MAX * sizeof(uint64_t));
    }

    bool ints = disable_interrupts();

    if (!ensure_wc_pat_slot()) {
        goto out;
    }

    uint64_t end = ALIGN_UP(CHECKED_ADD(base, size, goto out), 0x1000, goto out);
    base &= ~(uint64_t)0xfff;

    uint64_t cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *top = (uint64_t *)(cr3 & PT_ADDR_MASK);
    int levels = la57_enabled() ? 5 : 4;
    gib_supported = gib_pages_supported();

    uint64_t old_cr0;
    wp_off(&old_cr0);
    wc_walk(top, levels, 0, base, end, true);
    wp_on(old_cr0);

out:
    if (ints) {
        enable_interrupts();
    }
}

void efi_pt_restore(void) {
    if (saved_pte_i == 0 && !pat_modified) {
        goto out;
    }

    bool ints = disable_interrupts();

    if (saved_pte_i != 0) {
        uint64_t old_cr0;
        wp_off(&old_cr0);
        for (size_t i = saved_pte_i; i-- > 0;) {
            *saved_pte_ptr[i] = saved_pte_val[i];
        }
        wp_on(old_cr0);
    }

    if (pat_modified) {
        uint64_t old_cr0;
        cache_off(&old_cr0);
        wrmsr(IA32_PAT_MSR, saved_pat);
        cache_on(old_cr0);
    }

    if (ints) {
        enable_interrupts();
    }

    saved_pte_i = 0;
    pat_modified = false;
    wc_pat_index = -1;

out:
    if (saved_pte_ptr != NULL) {
        pmm_free(saved_pte_ptr, SAVED_PTES_MAX * sizeof(uint64_t *));
        pmm_free(saved_pte_val, SAVED_PTES_MAX * sizeof(uint64_t));
        saved_pte_ptr = NULL;
        saved_pte_val = NULL;
    }
}

#endif
