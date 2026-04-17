#include <stdint.h>
#include <stddef.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <sys/cpu.h>

#define PT_SIZE ((uint64_t)0x1000)

typedef uint64_t pt_entry_t;

// Maps level indexes to the page size for that level.
_Static_assert(VMM_MAX_LEVEL <= 5, "6-level paging not supported");
static uint64_t page_sizes[5] = {
    0x1000,
    0x200000,
    0x40000000,
    0x8000000000,
    0x1000000000000,
};

static pt_entry_t *get_next_level(pagemap_t pagemap, pt_entry_t *current_level,
                                  uint64_t virt, enum page_size desired_sz,
                                  size_t level_idx, size_t entry);

void map_pages(pagemap_t pagemap, uint64_t virt, uint64_t phys, uint64_t flags, uint64_t count) {
    if (virt % 0x1000 != 0 || phys % 0x1000 != 0 || count % 0x1000 != 0) {
        panic(true, "vmm: Misaligned call to map_pages()");
    }

    for (uint64_t i = 0; i < count; ) {
        if (((phys + i) & (0x40000000 - 1)) == 0 && ((virt + i) & (0x40000000 - 1)) == 0 && count - i >= 0x40000000) {
            map_page(pagemap, virt + i, phys + i, flags, Size1GiB);
            i += 0x40000000;
            continue;
        }
        if (((phys + i) & (0x200000 - 1)) == 0 && ((virt + i) & (0x200000 - 1)) == 0 && count - i >= 0x200000) {
            map_page(pagemap, virt + i, phys + i, flags, Size2MiB);
            i += 0x200000;
            continue;
        }
        map_page(pagemap, virt + i, phys + i, flags, Size4KiB);
        i += 0x1000;
    }
}

#if defined (__x86_64__) || defined (__i386__)

#define PT_FLAG_VALID    ((uint64_t)1 << 0)
#define PT_FLAG_WRITE    ((uint64_t)1 << 1)
#define PT_FLAG_USER     ((uint64_t)1 << 2)
#define PT_FLAG_LARGE    ((uint64_t)1 << 7)
#define PT_FLAG_NX       ((uint64_t)1 << 63)
#define PT_PADDR_MASK    ((uint64_t)0x0000FFFFFFFFF000)

#define PT_TABLE_FLAGS   (PT_FLAG_VALID | PT_FLAG_WRITE | PT_FLAG_USER)
#define PT_IS_TABLE(x) (((x) & (PT_FLAG_VALID | PT_FLAG_LARGE)) == PT_FLAG_VALID)
#define PT_IS_LARGE(x) (((x) & (PT_FLAG_VALID | PT_FLAG_LARGE)) == (PT_FLAG_VALID | PT_FLAG_LARGE))
#define PT_TO_VMM_FLAGS(x) ((x) & (PT_FLAG_WRITE | PT_FLAG_NX | VMM_FLAG_FB))

#define pte_new(addr, flags)    ((pt_entry_t)(addr) | (flags))
#define pte_addr(pte)           ((pte) & PT_PADDR_MASK)

pagemap_t new_pagemap(int paging_mode) {
    pagemap_t pagemap;
    pagemap.levels    = paging_mode == PAGING_MODE_X86_64_5LVL ? 5 : 4;
    pagemap.top_level = ext_mem_alloc(PT_SIZE);
    return pagemap;
}

static bool is_1gib_page_supported(void) {
    // Cache the cpuid result :^)
    static bool CACHE_INIT = false;
    static bool CACHE = false;

    if (!CACHE_INIT) {
        // Check if 1GiB pages are supported:
        uint32_t eax, ebx, ecx, edx;

        CACHE = cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx) && ((edx & 1 << 26) == 1 << 26);
        CACHE_INIT = true;

        printv("paging: 1GiB pages are %s!\n", CACHE ? "supported" : "not supported");
    }

    return CACHE;
}

void map_page(pagemap_t pagemap, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags, enum page_size pg_size) {
    // Calculate the indices in the various tables using the virtual address
    size_t pml5_entry = (virt_addr & ((uint64_t)0x1ff << 48)) >> 48;
    size_t pml4_entry = (virt_addr & ((uint64_t)0x1ff << 39)) >> 39;
    size_t pml3_entry = (virt_addr & ((uint64_t)0x1ff << 30)) >> 30;
    size_t pml2_entry = (virt_addr & ((uint64_t)0x1ff << 21)) >> 21;
    size_t pml1_entry = (virt_addr & ((uint64_t)0x1ff << 12)) >> 12;

    pt_entry_t *pml5, *pml4, *pml3, *pml2, *pml1;

    static bool pat_supported = false, pat_supported_got = false;
    if (!pat_supported_got) {
        uint32_t eax, ebx, ecx, edx;
        if (cpuid(1, 0, &eax, &ebx, &ecx, &edx) && (edx & (1 << 16))) {
            pat_supported = true;
        }
        pat_supported_got = true;
    }

    flags |= PT_FLAG_VALID; // Always present
    if ((flags & VMM_FLAG_FB) && !pat_supported) {
        flags &= ~(uint64_t)VMM_FLAG_FB;
    }

    // Paging levels
    switch (pagemap.levels) {
        case 5:
            pml5 = pagemap.top_level;
            goto level5;
        case 4:
            pml4 = pagemap.top_level;
            goto level4;
        default:
            __builtin_unreachable();
    }

level5:
    pml4 = get_next_level(pagemap, pml5, virt_addr, pg_size, 4, pml5_entry);
level4:
    pml3 = get_next_level(pagemap, pml4, virt_addr, pg_size, 3, pml4_entry);

    if (pg_size == Size1GiB) {
        // Check if 1GiB pages are available.
        if (is_1gib_page_supported()) {
            pml3[pml3_entry] = (pt_entry_t)(phys_addr | flags | PT_FLAG_LARGE);
        } else {
            // If 1GiB pages are not supported then emulate it by splitting them into
            // 2MiB pages.
            for (uint64_t i = 0; i < 0x40000000; i += 0x200000) {
                map_page(pagemap, virt_addr + i, phys_addr + i, flags, Size2MiB);
            }
        }

        return;
    }

    pml2 = get_next_level(pagemap, pml3, virt_addr, pg_size, 2, pml3_entry);

    if (pg_size == Size2MiB) {
        pml2[pml2_entry] = (pt_entry_t)(phys_addr | flags | PT_FLAG_LARGE);
        return;
    }

    pml1 = get_next_level(pagemap, pml2, virt_addr, pg_size, 1, pml2_entry);

    // PML1 wants PAT bit at 7 instead of 12
    if (flags & ((uint64_t)1 << 12)) {
        flags &= ~((uint64_t)1 << 12);
        flags |= ((uint64_t)1 << 7);
    }

    pml1[pml1_entry] = (pt_entry_t)(phys_addr | flags);
}

#elif defined (__aarch64__)

// Here we operate under the assumption that 4K pages are supported by the CPU.
// This appears to be guaranteed by UEFI, as section 2.3.6 "AArch64 Platforms"
// states that the primary processor core configuration includes 4K translation
// granules (TCR_EL1.TG0 = 0).
// Support for 4K pages also implies 2M, 1G and 512G blocks.

// Sanity check that 4K pages are supported.
void vmm_assert_4k_pages(void) {
    uint64_t aa64mmfr0;
    asm volatile ("mrs %0, id_aa64mmfr0_el1" : "=r"(aa64mmfr0));

    if (((aa64mmfr0 >> 28) & 0b1111) == 0b1111) {
        panic(false, "vmm: CPU does not support 4K pages, please make a bug report about this.");
    }
}

#define PT_FLAG_VALID    ((uint64_t)1 << 0)
#define PT_FLAG_TABLE    ((uint64_t)1 << 1)
#define PT_FLAG_4K_PAGE  ((uint64_t)1 << 1)
#define PT_FLAG_BLOCK    ((uint64_t)0 << 1)
#define PT_FLAG_USER     ((uint64_t)1 << 6)
#define PT_FLAG_READONLY ((uint64_t)1 << 7)
#define PT_FLAG_INNER_SH ((uint64_t)3 << 8)
#define PT_FLAG_ACCESS   ((uint64_t)1 << 10)
#define PT_FLAG_XN       ((uint64_t)1 << 54)
#define PT_FLAG_WB       ((uint64_t)0 << 2)
#define PT_FLAG_FB       ((uint64_t)1 << 2)
#define PT_PADDR_MASK    ((uint64_t)0x0000FFFFFFFFF000)

#define PT_TABLE_FLAGS   (PT_FLAG_VALID | PT_FLAG_TABLE)

#define PT_IS_TABLE(x) (((x) & (PT_FLAG_VALID | PT_FLAG_TABLE)) == (PT_FLAG_VALID | PT_FLAG_TABLE))
#define PT_IS_LARGE(x) (((x) & (PT_FLAG_VALID | PT_FLAG_TABLE)) == PT_FLAG_VALID)
#define PT_TO_VMM_FLAGS(x) (pt_to_vmm_flags_internal(x))

#define pte_new(addr, flags)    ((pt_entry_t)(addr) | (flags))
#define pte_addr(pte)           ((pte) & PT_PADDR_MASK)

static uint64_t pt_to_vmm_flags_internal(pt_entry_t entry) {
    uint64_t flags = 0;

    if (!(entry & PT_FLAG_READONLY))
        flags |= VMM_FLAG_WRITE;
    if (entry & PT_FLAG_XN)
        flags |= VMM_FLAG_NOEXEC;
    if (entry & PT_FLAG_FB)
        flags |= VMM_FLAG_FB;

    return flags;
}

pagemap_t new_pagemap(int paging_mode) {
    pagemap_t pagemap;
    pagemap.levels       = paging_mode == PAGING_MODE_AARCH64_5LVL ? 5 : 4;
    pagemap.top_level[0] = ext_mem_alloc(PT_SIZE);
    pagemap.top_level[1] = ext_mem_alloc(PT_SIZE);
    return pagemap;
}

void map_page(pagemap_t pagemap, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags, enum page_size pg_size) {
    // Calculate the indices in the various tables using the virtual address
    size_t pml5_entry = (virt_addr & ((uint64_t)0xf << 48)) >> 48;
    size_t pml4_entry = (virt_addr & ((uint64_t)0x1ff << 39)) >> 39;
    size_t pml3_entry = (virt_addr & ((uint64_t)0x1ff << 30)) >> 30;
    size_t pml2_entry = (virt_addr & ((uint64_t)0x1ff << 21)) >> 21;
    size_t pml1_entry = (virt_addr & ((uint64_t)0x1ff << 12)) >> 12;

    pt_entry_t *pml5, *pml4, *pml3, *pml2, *pml1;

    bool is_higher_half = virt_addr & ((uint64_t)1 << 63);

    uint64_t real_flags = PT_FLAG_VALID | PT_FLAG_INNER_SH | PT_FLAG_ACCESS | PT_FLAG_WB;
    if (!(flags & VMM_FLAG_WRITE))
        real_flags |= PT_FLAG_READONLY;
    if (flags & VMM_FLAG_NOEXEC)
        real_flags |= PT_FLAG_XN;
    if (flags & VMM_FLAG_FB)
        real_flags |= PT_FLAG_FB;

    // Paging levels
    switch (pagemap.levels) {
        case 5:
            pml5 = pagemap.top_level[is_higher_half];
            goto level5;
        case 4:
            pml4 = pagemap.top_level[is_higher_half];
            goto level4;
        default:
            __builtin_unreachable();
    }

level5:
    pml4 = get_next_level(pagemap, pml5, virt_addr, pg_size, 4, pml5_entry);
level4:
    pml3 = get_next_level(pagemap, pml4, virt_addr, pg_size, 3, pml4_entry);

    if (pg_size == Size1GiB) {
        pml3[pml3_entry] = (pt_entry_t)(phys_addr | real_flags | PT_FLAG_BLOCK);
        return;
    }

    pml2 = get_next_level(pagemap, pml3, virt_addr, pg_size, 2, pml3_entry);

    if (pg_size == Size2MiB) {
        pml2[pml2_entry] = (pt_entry_t)(phys_addr | real_flags | PT_FLAG_BLOCK);
        return;
    }

    pml1 = get_next_level(pagemap, pml2, virt_addr, pg_size, 1, pml2_entry);

    pml1[pml1_entry] = (pt_entry_t)(phys_addr | real_flags | PT_FLAG_4K_PAGE);
}

#elif defined (__riscv)

#define PT_FLAG_VALID       ((uint64_t)1 << 0)
#define PT_FLAG_READ        ((uint64_t)1 << 1)
#define PT_FLAG_WRITE       ((uint64_t)1 << 2)
#define PT_FLAG_EXEC        ((uint64_t)1 << 3)
#define PT_FLAG_USER        ((uint64_t)1 << 4)
#define PT_FLAG_ACCESSED    ((uint64_t)1 << 6)
#define PT_FLAG_DIRTY       ((uint64_t)1 << 7)
#define PT_FLAG_PBMT_NC     ((uint64_t)1 << 62)
#define PT_PADDR_MASK       ((uint64_t)0x003ffffffffffc00)

#define PT_FLAG_RWX         (PT_FLAG_READ | PT_FLAG_WRITE | PT_FLAG_EXEC)

#define PT_TABLE_FLAGS      PT_FLAG_VALID
#define PT_IS_TABLE(x)      (((x) & (PT_FLAG_VALID | PT_FLAG_RWX)) == PT_FLAG_VALID)
#define PT_IS_LARGE(x)      (((x) & (PT_FLAG_VALID | PT_FLAG_RWX)) > PT_FLAG_VALID)
#define PT_TO_VMM_FLAGS(x)  (pt_to_vmm_flags_internal(x))

#define pte_new(addr, flags)    ((((pt_entry_t)(addr) >> 2) & PT_PADDR_MASK) | (flags))
#define pte_addr(pte)           (((pte) & PT_PADDR_MASK) << 2)

static uint64_t pt_to_vmm_flags_internal(pt_entry_t entry) {
    uint64_t flags = 0;

    if (entry & PT_FLAG_WRITE)
        flags |= VMM_FLAG_WRITE;
    if (!(entry & PT_FLAG_EXEC))
        flags |= VMM_FLAG_NOEXEC;
    if (entry & PT_FLAG_PBMT_NC)
        flags |= VMM_FLAG_FB;

    return flags;
}

uint64_t paging_mode_higher_half(int paging_mode) {
    switch (paging_mode) {
        case PAGING_MODE_RISCV_SV39:
            return 0xffffffc000000000;
        case PAGING_MODE_RISCV_SV48:
            return 0xffff800000000000;
        case PAGING_MODE_RISCV_SV57:
            return 0xff00000000000000;
        default:
            panic(false, "paging_mode_higher_half: invalid mode");
    }
}

int paging_mode_va_bits(int paging_mode) {
    switch (paging_mode) {
        case PAGING_MODE_RISCV_SV39:
            return 39;
        case PAGING_MODE_RISCV_SV48:
            return 48;
        case PAGING_MODE_RISCV_SV57:
            return 57;
        default:
            panic(false, "paging_mode_va_bits: invalid mode");
    }
}

int vmm_max_paging_mode(void)
{
    if (bsp_hart == NULL || !(bsp_hart->flags & RISCV_HART_HAS_MMU)) {
        panic(false, "vmm: BSP hart does not advertise MMU support");
    }

    return PAGING_MODE_RISCV_SV39 + bsp_hart->mmu_type;
}

static pt_entry_t pbmt_nc = 0;

pagemap_t new_pagemap(int paging_mode) {
    pagemap_t pagemap;
    pagemap.paging_mode   = paging_mode;
    pagemap.max_page_size = paging_mode - 6;
    pagemap.top_level     = ext_mem_alloc(PT_SIZE);

    if (riscv_check_isa_extension("svpbmt", NULL, NULL)) {
        printv("riscv: Svpbmt extension is supported.\n");
        pbmt_nc = PT_FLAG_PBMT_NC;
    }

    return pagemap;
}

void map_page(pagemap_t pagemap, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags, enum page_size page_size) {
    // Truncate the requested page size to the maximum supported.
    if (page_size > pagemap.max_page_size)
        page_size = pagemap.max_page_size;

    // Convert VMM_FLAG_* into PT_FLAG_*.
    // Set the ACCESSED and DIRTY flags to avoid faults.
    pt_entry_t ptflags = PT_FLAG_VALID | PT_FLAG_READ | PT_FLAG_ACCESSED | PT_FLAG_DIRTY;
    if (flags & VMM_FLAG_WRITE)
        ptflags |= PT_FLAG_WRITE;
    if (!(flags & VMM_FLAG_NOEXEC))
        ptflags |= PT_FLAG_EXEC;
    if (flags & VMM_FLAG_FB)
        ptflags |= pbmt_nc;

    // Start at the highest level.
    // The values of `enum page_size` map to the level index at which that size is mapped.
    int level = pagemap.max_page_size;
    pt_entry_t *table = pagemap.top_level;
    for (;;) {
        int index = (virt_addr >> (12 + 9 * level)) & 0x1ff;

        // Stop when we reach the level for the requested page size.
        if (level == (int)page_size) {
            table[index] = pte_new(phys_addr, ptflags);
            break;
        }

        table = get_next_level(pagemap, table, virt_addr, page_size, level, index);
        level--;
    }
}

#elif defined (__loongarch64)

#define INVALID_PAGE    0

#define PT_FLAG_VALID   ((uint64_t)1 << 0)
#define PT_FLAG_DIRTY   ((uint64_t)1 << 1)
#define PT_FLAG_MAT_CC  ((uint64_t)1 << 4)
#define PT_FLAG_MAT_WUC ((uint64_t)1 << 5)
#define PT_FLAG_GLOBAL  ((uint64_t)1 << 6)
#define PT_FLAG_HUGE    ((uint64_t)1 << 6)
#define PT_FLAG_WRITE   ((uint64_t)1 << 8)
#define PT_FLAG_HGLOBAL ((uint64_t)1 << 12)
#define PT_FLAG_NX      ((uint64_t)1 << 62)
#define PT_PADDR_MASK   ((uint64_t)0x0000FFFFFFFFF000)
#define PT_PADDR_HMASK  ((uint64_t)0x0000FFFFFF000000)

#define PT_TABLE_FLAGS      0
#define PT_IS_TABLE(x)      ((level_idx > 0) && (((x) & PT_FLAG_VALID) == 0) && ((x) != INVALID_PAGE))
#define PT_IS_LARGE(x)      (((x) & (PT_FLAG_HGLOBAL | PT_FLAG_HUGE)) == (PT_FLAG_HGLOBAL | PT_FLAG_HUGE))
#define PT_TO_VMM_FLAGS(x)  (pt_to_vmm_flags_internal(x))

#define pte_new(addr, flags)    (pt_entry_t)((addr) | (flags))

static inline uint64_t pte_addr(uint64_t pte) {
    if (PT_IS_LARGE(pte)) {
        return pte & PT_PADDR_HMASK;
    }
    return pte & PT_PADDR_MASK;
}

static uint64_t pt_to_vmm_flags_internal(pt_entry_t entry) {
    uint64_t flags = 0;

    if (entry & PT_FLAG_WRITE)
        flags |= VMM_FLAG_WRITE;
    if (entry & PT_FLAG_NX)
        flags |= VMM_FLAG_NOEXEC;
    if (entry & PT_FLAG_MAT_WUC)
        flags |= VMM_FLAG_FB;

    return flags;
}

pagemap_t new_pagemap(int paging_mode) {
    (void)paging_mode;

    uint32_t cpucfg_val;

    asm volatile (
        "cpucfg %0, %1"
        : "=r"(cpucfg_val)
        : "r"(1)
    );

    uint32_t valen = ((cpucfg_val >> 12) & 0xff) + 1;
    if (valen < 48) {
        panic(true, "vmm: VALEN values < 48 not currently supported");
    }

    (void)paging_mode;
    pagemap_t pagemap;
    pagemap.pgd[0] = ext_mem_alloc(PT_SIZE);
    pagemap.pgd[1] = ext_mem_alloc(PT_SIZE);
    return pagemap;
}

void map_page(pagemap_t pagemap, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags, enum page_size pg_size) {
    size_t pml4_entry = (virt_addr & ((uint64_t)0x1ff << 39)) >> 39;
    size_t pml3_entry = (virt_addr & ((uint64_t)0x1ff << 30)) >> 30;
    size_t pml2_entry = (virt_addr & ((uint64_t)0x1ff << 21)) >> 21;
    size_t pml1_entry = (virt_addr & ((uint64_t)0x1ff << 12)) >> 12;

    pt_entry_t *pml4, *pml3, *pml2, *pml1;

    bool is_higher_half = virt_addr & ((uint64_t)1 << 63);

    uint64_t real_flags = PT_FLAG_VALID | PT_FLAG_GLOBAL;
    if (flags & VMM_FLAG_WRITE)
        real_flags |= PT_FLAG_DIRTY | PT_FLAG_WRITE;
    if (flags & VMM_FLAG_NOEXEC)
        real_flags |= PT_FLAG_NX;
    if (flags & VMM_FLAG_FB)
        real_flags |= PT_FLAG_MAT_WUC;
    else
        real_flags |= PT_FLAG_MAT_CC;

    pml4 = pagemap.pgd[is_higher_half];

    pml3 = get_next_level(pagemap, pml4, virt_addr, pg_size, 3, pml4_entry);

    if (pg_size == Size1GiB) {
        pml3[pml3_entry] = pte_new(phys_addr, real_flags | PT_FLAG_HGLOBAL | PT_FLAG_HUGE);
        return;
    }

    pml2 = get_next_level(pagemap, pml3, virt_addr, pg_size, 2, pml3_entry);

    if (pg_size == Size2MiB) {
        pml2[pml2_entry] = pte_new(phys_addr, real_flags | PT_FLAG_HGLOBAL | PT_FLAG_HUGE);
        return;
    }

    pml1 = get_next_level(pagemap, pml2, virt_addr, pg_size, 1, pml2_entry);

    pml1[pml1_entry] = pte_new(phys_addr, real_flags);
}

#else
#error Unknown architecture
#endif

static pt_entry_t *get_next_level(pagemap_t pagemap, pt_entry_t *current_level,
                                  uint64_t virt, enum page_size desired_sz,
                                  size_t level_idx, size_t entry) {
    pt_entry_t *ret;

    if (PT_IS_TABLE(current_level[entry])) {
        ret = (pt_entry_t *)(size_t)pte_addr(current_level[entry]);
    } else {
        if (PT_IS_LARGE(current_level[entry])) {
            // We are replacing an existing large page with a smaller page.
            // Split the previous mapping into mappings of the newly requested size
            // before performing the requested map operation.


            if ((level_idx >= VMM_MAX_LEVEL) || (level_idx == 0))
                panic(false, "Unexpected level in get_next_level");
            if (desired_sz >= VMM_MAX_LEVEL)
                panic(false, "Unexpected page size in get_next_level");

            uint64_t old_page_size = page_sizes[level_idx];
            uint64_t new_page_size = page_sizes[desired_sz];

            // Save all the information from the old entry at this level
            uint64_t old_flags = PT_TO_VMM_FLAGS(current_level[entry]);
            uint64_t old_phys = pte_addr(current_level[entry]);
            uint64_t old_virt = virt & ~(old_page_size - 1);

            if (old_phys & (old_page_size - 1))
                panic(false, "Unexpected page table entry address in get_next_level");

            // Allocate a table for the next level
            ret = ext_mem_alloc(PT_SIZE);
            current_level[entry] = pte_new((size_t)ret, PT_TABLE_FLAGS);

            // Recreate the old mapping with smaller pages
            for (uint64_t i = 0; i < old_page_size; i += new_page_size) {
                map_page(pagemap, old_virt + i, old_phys + i, old_flags, desired_sz);
            }
        } else {
            // Allocate a table for the next level
            ret = ext_mem_alloc(PT_SIZE);
            current_level[entry] = pte_new((size_t)ret, PT_TABLE_FLAGS);
        }
    }

    return ret;
}
