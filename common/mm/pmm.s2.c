#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <mm/pmm.h>
#include <sys/e820.h>
#include <lib/acpi.h>
#include <lib/misc.h>
#include <lib/libc.h>
#include <lib/print.h>
#if defined (UEFI)
#  include <efi.h>
#endif

#define PAGE_SIZE   4096

#if defined (BIOS)
extern symbol bss_end;
#endif

bool allocations_disallowed = true;

void *conv_mem_alloc(uint64_t count) {
    static uint64_t base = 4096;

    if (allocations_disallowed)
        panic(false, "Memory allocations disallowed");

    count = ALIGN_UP(count, 4096, panic(false, "Alignment overflow"));

    for (;;) {
        if (base + count > 0x100000)
            panic(false, "Conventional memory allocation failed");

        if (memmap_alloc_range(base, count, MEMMAP_BOOTLOADER_RECLAIMABLE, MEMMAP_USABLE, false, false, false)) {
            void *ret = (void *)(uintptr_t)base;
            // Zero out allocated space
            memset(ret, 0, count);
            base += count;

            pmm_sanitise_entries(memmap, &memmap_entries, false);

            return ret;
        }

        base += 4096;
    }
}

#if defined (BIOS)
#define memmap_max_entries ((size_t)512)

struct memmap_entry memmap[memmap_max_entries];
size_t memmap_entries = 0;
#endif

#if defined (UEFI)
static size_t memmap_max_entries;

struct memmap_entry *memmap;
size_t memmap_entries = 0;

struct memmap_entry *untouched_memmap;
size_t untouched_memmap_entries = 0;
#endif

static const char *memmap_type(uint32_t type) {
    switch (type) {
        case MEMMAP_USABLE:
            return "Usable RAM";
        case MEMMAP_RESERVED:
            return "Reserved";
        case MEMMAP_RESERVED_MAPPED:
            return "Reserved (Mapped)";
        case MEMMAP_ACPI_RECLAIMABLE:
            return "ACPI reclaimable";
        case MEMMAP_ACPI_NVS:
            return "ACPI NVS";
        case MEMMAP_BAD_MEMORY:
            return "Bad memory";
        case MEMMAP_FRAMEBUFFER:
            return "Framebuffer";
        case MEMMAP_BOOTLOADER_RECLAIMABLE:
            return "Bootloader reclaimable";
        case MEMMAP_KERNEL_AND_MODULES:
            return "Kernel/Modules";
        case MEMMAP_EFI_RECLAIMABLE:
            return "EFI reclaimable";
        default:
            return "???";
    }
}

void print_memmap(struct memmap_entry *mm, size_t size) {
    for (size_t i = 0; i < size; i++) {
        print("[%X -> %X] : %X  <%s (%x)>\n",
              mm[i].base,
              mm[i].base + mm[i].length,
              mm[i].length,
              memmap_type(mm[i].type), mm[i].type);
    }
}

static bool align_entry(uint64_t *base, uint64_t *length) {
    if (*length < PAGE_SIZE)
        return false;

    uint64_t orig_base = *base;

    *base = ALIGN_UP(*base, PAGE_SIZE, return false);

    *length -= (*base - orig_base);
    *length =  ALIGN_DOWN(*length, PAGE_SIZE);

    if (*length == 0)
        return false;

    return true;
}

#if defined (BIOS)
bool pmm_sanitiser_keep_first_page = false;
#else
bool pmm_sanitiser_keep_first_page = true;
#endif

void pmm_sanitise_entries(struct memmap_entry *m, size_t *_count, bool align_entries) {
    size_t count = *_count;

    for (size_t i = 0; i < count; i++) {
        if (m[i].type != MEMMAP_USABLE)
            continue;

        // Check if the entry overlaps other entries
        for (size_t j = 0; j < count; j++) {
            if (j == i)
                continue;

            uint64_t base   = m[i].base;
            uint64_t length = m[i].length;
            uint64_t top    = CHECKED_ADD(base, length, goto del_mm0);

            uint64_t res_base   = m[j].base;
            uint64_t res_length = m[j].length;
            uint64_t res_top    = CHECKED_ADD(res_base, res_length, continue);

            // Non-usable entry fully contains usable entry
            if (res_base <= base && res_top >= top) {
                m[i].base   = top;
                m[i].length = 0;
                break;
            }

            if ( (res_base >= base && res_base < top)
              && (res_top  >= base && res_top  < top) ) {
                // TODO actually handle splitting off usable chunks
                panic(false, "A non-usable memory map entry is inside a usable section.");
            }

            if (res_base >= base && res_base < top) {
                top = res_base;
            }

            if (res_top  >= base && res_top  < top) {
                base = res_top;
            }

            m[i].base   = base;
            m[i].length = top - base;
        }

        if (!m[i].length
         || (align_entries && !align_entry(&m[i].base, &m[i].length))) {
del_mm0:
            // Remove i from memmap
            if (i < count - 1) {
                m[i] = m[count - 1];
            }
            count--; i = (size_t)-1; // restart outer loop
        }
    }

    // Remove 0 length usable entries and usable entries below 0x1000
    for (size_t i = 0; i < count; i++) {
        if (m[i].type != MEMMAP_USABLE)
            continue;

        if (!pmm_sanitiser_keep_first_page && m[i].base < 0x1000) {
            uint64_t entry_top = CHECKED_ADD(m[i].base, m[i].length, goto del_mm1);
            if (entry_top <= 0x1000) {
                goto del_mm1;
            }

            m[i].length -= 0x1000 - m[i].base;
            m[i].base = 0x1000;
        }

        if (m[i].length == 0) {
del_mm1:
            // Remove i from memmap
            if (i < count - 1) {
                m[i] = m[count - 1];
            }
            count--; i--;
        }
    }

    // Sort the entries
    for (size_t p = 0; p + 1 < count; p++) {
        uint64_t min = m[p].base;
        size_t min_index = p;
        for (size_t i = p; i < count; i++) {
            if (m[i].base < min) {
                min = m[i].base;
                min_index = i;
            }
        }
        struct memmap_entry min_e = m[min_index];
        m[min_index] = m[p];
        m[p] = min_e;
    }

    // Merge contiguous bootloader-reclaimable, reserved (mapped), usable entries
    for (size_t i = 0; i + 1 < count; i++) {
        if (m[i].type != MEMMAP_BOOTLOADER_RECLAIMABLE
         && m[i].type != MEMMAP_RESERVED_MAPPED
         && m[i].type != MEMMAP_USABLE)
            continue;

        uint64_t merge_top = CHECKED_ADD(m[i].base, m[i].length, continue);
        if (m[i+1].type == m[i].type
         && m[i+1].base == merge_top) {
            m[i].length = CHECKED_ADD(m[i].length, m[i+1].length, continue);

            // Eradicate from memmap
            for (size_t j = i + 2; j < count; j++) {
                m[j - 1] = m[j];
            }
            count--;
            i--;
        }
    }

    *_count = count;
}

#if defined (UEFI)
static void pmm_reclaim_uefi_mem(struct memmap_entry *m, size_t *_count, bool raw);
#endif

struct memmap_entry *get_memmap(size_t *entries) {
#if defined (UEFI)
    if (efi_boot_services_exited == false) {
        panic(true, "get_memmap called whilst in boot services");
    }

    pmm_reclaim_uefi_mem(memmap, &memmap_entries, false);
#endif

    pmm_sanitise_entries(memmap, &memmap_entries, true);

    *entries = memmap_entries;

    allocations_disallowed = true;

    return memmap;
}

#if defined (BIOS)
void init_memmap(void) {
    for (size_t i = 0; i < e820_entries; i++) {
        if (memmap_entries == memmap_max_entries) {
            panic(false, "Memory map exhausted.");
        }

        memmap[memmap_entries] = e820_map[i];

        uint64_t top = CHECKED_ADD(memmap[memmap_entries].base, memmap[memmap_entries].length, continue);

        if (memmap[memmap_entries].type == MEMMAP_USABLE) {
            if (memmap[memmap_entries].base >= EBDA && memmap[memmap_entries].base < 0x100000) {
                if (top <= 0x100000)
                    continue;

                memmap[memmap_entries].length -= 0x100000 - memmap[memmap_entries].base;
                memmap[memmap_entries].base = 0x100000;
            }

            if (top > EBDA && top <= 0x100000) {
                memmap[memmap_entries].length -= top - EBDA;
            }
        }

        memmap_entries++;
    }

    pmm_sanitise_entries(memmap, &memmap_entries, false);

    // Allocate bootloader itself
    memmap_alloc_range(4096,
        ALIGN_UP((uintptr_t)bss_end, 4096, panic(false, "Alignment overflow")) - 4096, MEMMAP_BOOTLOADER_RECLAIMABLE, 0, true, false, false);

    pmm_sanitise_entries(memmap, &memmap_entries, false);

    allocations_disallowed = false;
}
#endif

#if defined (UEFI)
static struct memmap_entry *recl;

extern symbol __slide, __image_base, __image_end;

void init_memmap(void) {
    EFI_STATUS status;

    EFI_MEMORY_DESCRIPTOR tmp_mmap[1];
    efi_mmap_size = sizeof(tmp_mmap);
    UINTN mmap_key = 0;

    gBS->GetMemoryMap(&efi_mmap_size, tmp_mmap, &mmap_key, &efi_desc_size, &efi_desc_ver);

    memmap_max_entries = (efi_mmap_size / efi_desc_size) + 512;

    efi_mmap_size += 4096;

    status = gBS->AllocatePool(EfiLoaderData, efi_mmap_size, (void **)&efi_mmap);
    if (status) {
        goto fail;
    }

    size_t memmap_alloc_size = CHECKED_MUL(memmap_max_entries, sizeof(struct memmap_entry), goto fail);

    status = gBS->AllocatePool(EfiLoaderData, memmap_alloc_size, (void **)&memmap);
    if (status) {
        gBS->FreePool(efi_mmap);
        goto fail;
    }

    status = gBS->AllocatePool(EfiLoaderData, memmap_alloc_size, (void **)&untouched_memmap);
    if (status) {
        gBS->FreePool(efi_mmap);
        gBS->FreePool(memmap);
        goto fail;
    }

    status = gBS->GetMemoryMap(&efi_mmap_size, efi_mmap, &mmap_key, &efi_desc_size, &efi_desc_ver);
    if (status) {
        gBS->FreePool(efi_mmap);
        gBS->FreePool(memmap);
        gBS->FreePool(untouched_memmap);
        goto fail;
    }

    size_t entry_count = efi_mmap_size / efi_desc_size;

    for (size_t i = 0; i < entry_count; i++) {
        EFI_MEMORY_DESCRIPTOR *entry = (void *)efi_mmap + i * efi_desc_size;

        if (entry->NumberOfPages == 0) {
            continue;
        }

        uint32_t our_type;
        switch (entry->Type) {
            case EfiReservedMemoryType:
            case EfiRuntimeServicesCode:
            case EfiRuntimeServicesData:
            case EfiUnusableMemory:
            case EfiMemoryMappedIO:
            case EfiMemoryMappedIOPortSpace:
            case EfiPalCode:
            default:
                our_type = MEMMAP_RESERVED; break;
            case EfiLoaderCode:
            case EfiLoaderData:
            case EfiBootServicesCode:
            case EfiBootServicesData:
                our_type = MEMMAP_EFI_RECLAIMABLE; break;
            case EfiACPIReclaimMemory:
                our_type = MEMMAP_ACPI_RECLAIMABLE; break;
            case EfiACPIMemoryNVS:
                our_type = MEMMAP_ACPI_NVS; break;
            case EfiConventionalMemory:
                our_type = MEMMAP_USABLE; break;
        }

        uint64_t base = entry->PhysicalStart;
        uint64_t length = CHECKED_MUL(entry->NumberOfPages, 4096, continue);

        if (memmap_entries == memmap_max_entries) {
            panic(false, "Memory map exhausted.");
        }

        memmap[memmap_entries].base = base;
        memmap[memmap_entries].length = length;
        memmap[memmap_entries].type = our_type;

        memmap_entries++;
    }

    pmm_sanitise_entries(memmap, &memmap_entries, false);

    allocations_disallowed = false;

    // Let's leave 64MiB to the firmware below 4GiB
    for (size_t i = 0; i < 64; i++) {
        ext_mem_alloc_type(0x100000, MEMMAP_EFI_RECLAIMABLE);
    }

    memcpy(untouched_memmap, memmap, memmap_entries * sizeof(struct memmap_entry));
    untouched_memmap_entries = memmap_entries;

    // Now own all the usable entries
    for (size_t i = 0; i < untouched_memmap_entries; i++) {
        if (untouched_memmap[i].type != MEMMAP_USABLE)
            continue;

        EFI_PHYSICAL_ADDRESS base = untouched_memmap[i].base;

#if defined (__i386__)
        if (CHECKED_ADD(untouched_memmap[i].base, untouched_memmap[i].length, continue) > 0x100000000) {
            continue;
        }
#endif

        status = gBS->AllocatePages(AllocateAddress, EfiLoaderCode,
                                    untouched_memmap[i].length / 4096, &base);

        if (status) {
            for (size_t j = 0; j < untouched_memmap[i].length; j += 4096) {
                base = untouched_memmap[i].base + j;
                status = gBS->AllocatePages(AllocateAddress, EfiLoaderCode, 1, &base);
                if (status) {
                    memmap_alloc_range(base, 4096, MEMMAP_EFI_RECLAIMABLE, MEMMAP_USABLE, true, false, false);
                }
            }
        }
    }

    memcpy(untouched_memmap, memmap, memmap_entries * sizeof(struct memmap_entry));
    untouched_memmap_entries = memmap_entries;

    // Allocate bootloader itself
    size_t image_size = ALIGN_UP((uintptr_t)__image_end - (uintptr_t)__image_base, 4096, panic(false, "Alignment overflow"));

    memmap_alloc_range((uintptr_t)__slide, (uintptr_t)image_size,
                       MEMMAP_BOOTLOADER_RECLAIMABLE, 0, true, false, true);

    pmm_sanitise_entries(memmap, &memmap_entries, false);

    recl = ext_mem_alloc_counted(1024, sizeof(struct memmap_entry));

    return;

fail:
    panic(false, "pmm: Failure initialising memory map");
}

static void pmm_reclaim_uefi_mem(struct memmap_entry *m, size_t *_count, bool raw) {
    size_t count = *_count;

    size_t recl_i = 0;

    for (size_t i = 0; i < count; i++) {
        if (m[i].type == MEMMAP_EFI_RECLAIMABLE) {
            if (recl_i >= 1024) {
                panic(false, "pmm: Too many EFI reclaimable entries");
            }
            recl[recl_i++] = m[i];
        }
    }

    for (size_t ri = 0; ri < recl_i; ri++) {
        struct memmap_entry *r = &recl[ri];

        // Punch holes in our EFI reclaimable entry for every EFI area which is
        // boot services or conventional that fits within
        size_t efi_mmap_entry_count = efi_mmap_size / efi_desc_size;
        for (size_t i = 0; i < efi_mmap_entry_count; i++) {
            EFI_MEMORY_DESCRIPTOR *entry = (void *)efi_mmap + i * efi_desc_size;

            uint64_t base = r->base;
            uint64_t top = CHECKED_ADD(base, r->length, continue);
            uint64_t efi_base = entry->PhysicalStart;
            uint64_t efi_size = CHECKED_MUL(entry->NumberOfPages, 4096, continue);

            if (efi_base < base) {
                if (efi_size <= base - efi_base)
                    continue;
                efi_size -= base - efi_base;
                efi_base = base;
            }

            uint64_t efi_top = CHECKED_ADD(efi_base, efi_size, continue);

            if (efi_top > top) {
                if (efi_size <= efi_top - top)
                    continue;
                efi_size -= efi_top - top;
                efi_top = top;
            }

            // Sanity check
            if (!(efi_base >= base && efi_base <  top
               && efi_top  >  base && efi_top  <= top))
                continue;

            uint32_t our_type;
            switch (entry->Type) {
                case EfiLoaderCode:
                case EfiLoaderData:
                case EfiBootServicesCode:
                case EfiBootServicesData:
                    if (raw) {
                        our_type = MEMMAP_USABLE;
                    } else {
                        our_type = MEMMAP_BOOTLOADER_RECLAIMABLE;
                    }
                    break;
                case EfiConventionalMemory:
                    our_type = MEMMAP_USABLE; break;
                case EfiACPIReclaimMemory:
                    our_type = MEMMAP_ACPI_RECLAIMABLE; break;
                case EfiACPIMemoryNVS:
                    our_type = MEMMAP_ACPI_NVS; break;
                default:
                    our_type = MEMMAP_RESERVED; break;
            }

            memmap_alloc_range_in(m, &count, efi_base, efi_size, our_type, 0, true, false, false);
        }
    }

    allocations_disallowed = true;

    pmm_sanitise_entries(m, &count, false);

    *_count = count;
}

void pmm_release_uefi_mem(void) {
    EFI_STATUS status;

    for (size_t i = 0; i < untouched_memmap_entries; i++) {
        if (untouched_memmap[i].type != MEMMAP_USABLE
         && untouched_memmap[i].type != MEMMAP_BOOTLOADER_RECLAIMABLE) {
            continue;
        }

        status = gBS->FreePages(untouched_memmap[i].base, untouched_memmap[i].length / 4096);

        if (status) {
            panic(false, "pmm: FreePages failure (%X)", (uint64_t)status);
        }
    }

    allocations_disallowed = true;
}
#endif

#if defined (BIOS)
struct memmap_entry *get_raw_memmap(size_t *entry_count) {
    *entry_count = e820_entries;
    return e820_map;
}
#endif

#if defined (UEFI)
struct memmap_entry *get_raw_memmap(size_t *entry_count) {
    if (efi_boot_services_exited == false) {
        panic(true, "get_raw_memmap called whilst in boot services");
    }

    bool old_skfp = pmm_sanitiser_keep_first_page;
    pmm_sanitiser_keep_first_page = true;
    pmm_reclaim_uefi_mem(untouched_memmap, &untouched_memmap_entries, true);
    pmm_sanitiser_keep_first_page = old_skfp;

    *entry_count = untouched_memmap_entries;
    return untouched_memmap;
}
#endif

void pmm_free_size_t(void *ptr, size_t length) {
    pmm_free(ptr, length);
}

void pmm_free(void *ptr, uint64_t count) {
    if ((uintptr_t)ptr % 4096 != 0)
        panic(false, "pmm_free: Unaligned pointer %p", ptr);
    count = ALIGN_UP(count, 4096, panic(false, "Alignment overflow"));
    if (allocations_disallowed)
        panic(false, "Memory allocations disallowed");
    memmap_alloc_range((uintptr_t)ptr, count, MEMMAP_USABLE, 0, false, false, true);
}

void *pmm_realloc(void *old_ptr, uint64_t old_size, uint64_t new_size) {
    if (new_size == 0) {
        if (old_ptr != NULL) {
            pmm_free(old_ptr, old_size);
        }
        return NULL;
    }
    if (old_ptr == NULL) {
        return ext_mem_alloc(new_size);
    }

    void *new_ptr = ext_mem_alloc(new_size);

    memcpy(new_ptr, old_ptr, MIN(new_size, old_size));

    pmm_free(old_ptr, old_size);

    return new_ptr;
}

void *ext_mem_alloc_size_t(size_t count) {
    return ext_mem_alloc(count);
}

void *ext_mem_alloc(uint64_t count) {
    return ext_mem_alloc_type(count, MEMMAP_BOOTLOADER_RECLAIMABLE);
}

void *ext_mem_alloc_counted(uint64_t count, uint64_t elem_size) {
    return ext_mem_alloc(CHECKED_MUL(count, elem_size,
        panic(false, "ext_mem_alloc_counted: allocation size overflow")));
}

void *ext_mem_alloc_type(uint64_t count, uint32_t type) {
    return ext_mem_alloc_type_aligned(count, type, 4096);
}

void *ext_mem_alloc_type_aligned(uint64_t count, uint32_t type, size_t alignment) {
    return ext_mem_alloc_type_aligned_mode(count, type, alignment, false);
}

// Allocate memory top down.
void *ext_mem_alloc_type_aligned_mode(uint64_t count, uint32_t type, size_t alignment, bool allow_high_allocs) {
#if !defined (__x86_64__) && !defined (__i386__)
    (void)allow_high_allocs;
#endif

    count = CHECKED_ADD(count, alignment - 1,
        panic(false, "ext_mem_alloc: count overflows when aligning"));
    count = ALIGN_DOWN(count, alignment);

    if (allocations_disallowed)
        panic(false, "Memory allocations disallowed");

#if defined(__x86_64__) || defined(__i386__)
    // Try below 4GiB first to avoid relying on firmware identity-mapping
    // memory above 4GiB (some buggy UEFI implementations don't).
    uint64_t limit = 0x100000000;

again:
#else
    uint64_t limit = UINT64_MAX;
#endif

    for (int i = memmap_entries - 1; i >= 0; i--) {
        if (memmap[i].type != 1)
            continue;

        uint64_t entry_base = memmap[i].base;
        uint64_t entry_top  = CHECKED_ADD(memmap[i].base, memmap[i].length, continue);

        if (entry_top > limit) {
            entry_top = limit;
            if (entry_base >= entry_top)
                continue;
        }

        // Check if entry is too small before subtracting.
        if (entry_top - entry_base < count)
            continue;

        uint64_t alloc_base = ALIGN_DOWN(entry_top - count, alignment);

        if (alloc_base < entry_base)
            continue;

        // We now reserve the range we need.
        uint64_t aligned_length = entry_top - alloc_base;
        memmap_alloc_range(alloc_base, aligned_length, type, MEMMAP_USABLE, true, false, false);

        void *ret;

#if defined (__i386__)
        if (!allow_high_allocs) {
#endif
        ret = (void *)(size_t)alloc_base;

        // Zero out allocated space
        memset(ret, 0, count);
#if defined (__i386__)
        } else {
            static uint64_t above64_ret;
            above64_ret = alloc_base;
            ret = &above64_ret;
        }
#endif

        pmm_sanitise_entries(memmap, &memmap_entries, false);

        return ret;
    }

#if defined(__x86_64__) || defined(__i386__)
    if (allow_high_allocs && limit < UINT64_MAX) {
        limit = UINT64_MAX;
        goto again;
    }
#endif

    panic(false, "High memory allocator: Out of memory");
}

/// Compute and returns the amount of upper and lower memory till
/// the first hole.
struct meminfo mmap_get_info(size_t mmap_count, struct memmap_entry *mmap) {
    struct meminfo info = {0};

    // Find contiguous usable memory from address 0 (lower) and 1 MiB (upper)
    // by iteratively extending a frontier. Handles unsorted entries.
    uint64_t lower_end = 0;
    uint64_t upper_end = 0x100000;
    bool progress;

    do {
        progress = false;
        for (size_t i = 0; i < mmap_count; i++) {
            if (mmap[i].type != MEMMAP_USABLE)
                continue;
            uint64_t base = mmap[i].base;
            uint64_t top = CHECKED_ADD(base, mmap[i].length, continue);
            if (base <= lower_end && top > lower_end) {
                lower_end = top;
                progress = true;
            }
            if (base <= upper_end && top > upper_end) {
                upper_end = top;
                progress = true;
            }
        }
    } while (progress);

    if (lower_end > 0x100000)
        lower_end = 0x100000;

    info.lowermem = lower_end;
    info.uppermem = upper_end - 0x100000;

    return info;
}

static bool pmm_new_entry(struct memmap_entry *m, size_t *_count,
                          uint64_t base, uint64_t length, uint32_t type) {
    size_t count = *_count;

    uint64_t top = CHECKED_ADD(base, length, panic(false, "pmm: Integer overflow in memory range calculation"));

    // Handle overlapping new entries.
    for (size_t i = 0; i < count; i++) {
        uint64_t entry_base = m[i].base;
        uint64_t entry_top = CHECKED_ADD(m[i].base, m[i].length, continue);

        // Full overlap
        if (base <= entry_base && top >= entry_top) {
            // Remove overlapped entry
            if (i < count - 1) {
                m[i] = m[count - 1];
            }
            count--;
            i--;
            continue;
        }

        // Partial overlap (bottom)
        if (base <= entry_base && top < entry_top && top > entry_base) {
            // Entry gets bottom shaved off
            m[i].base += top - entry_base;
            m[i].length -= top - entry_base;
            continue;
        }

        // Partial overlap (top)
        if (base > entry_base && base < entry_top && top >= entry_top) {
            // Entry gets top shaved off
            m[i].length -= entry_top - base;
            continue;
        }

        // Nested (pain)
        if (base > entry_base && top < entry_top) {
            // Entry gets top shaved off first
            m[i].length -= entry_top - base;

            // Now we need to create a new entry
            if (count >= memmap_max_entries)
                panic(false, "Memory map exhausted.");

            struct memmap_entry *new_entry = &m[count++];

            new_entry->type = m[i].type;
            new_entry->base = top;
            new_entry->length = entry_top - top;

            continue;
        }
    }

    if (count >= memmap_max_entries)
        panic(false, "Memory map exhausted.");

    struct memmap_entry *target = &m[count++];

    target->type = type;
    target->base = base;
    target->length = length;

    *_count = count;
    return true;
}

uint64_t pmm_check_type(uint64_t addr) {
    for (size_t i = 0; i < memmap_entries; i++) {
        uint64_t entry_base = memmap[i].base;
        uint64_t entry_top  = CHECKED_ADD(memmap[i].base, memmap[i].length, continue);

        if (addr >= entry_base && addr < entry_top) {
            return memmap[i].type;
        }
    }

    return (uint64_t)-1;
}

bool memmap_alloc_range_in(struct memmap_entry *m, size_t *_count,
                           uint64_t base, uint64_t length, uint32_t type, uint32_t overlay_type, bool do_panic, bool simulation, bool new_entry) {
    size_t count = *_count;

    if (length == 0)
        return true;

    if (simulation && new_entry) {
        return true;
    }

    uint64_t top = CHECKED_ADD(base, length, ({
        if (do_panic)
            panic(false, "Memory allocation overflow.");
        return false;
    }));

    for (size_t i = 0; i < count; i++) {
        if (overlay_type != 0 && m[i].type != overlay_type)
            continue;

        uint64_t entry_base = m[i].base;
        uint64_t entry_top = CHECKED_ADD(m[i].base, m[i].length, continue);

        if (base >= entry_base && base < entry_top && top <= entry_top) {
            if (simulation)
                return true;

            if (pmm_new_entry(m, &count, base, length, type) == true) {
                goto success;
            }
        }
    }

    if (!new_entry && do_panic)
        panic(false, "Memory allocation failure.");

    if (!new_entry) {
        return false;
    }

    if (pmm_new_entry(m, &count, base, length, type) == false) {
        return false;
    }

success:
    pmm_sanitise_entries(m, &count, false);
    *_count = count;
    return true;
}

bool memmap_alloc_range(uint64_t base, uint64_t length, uint32_t type, uint32_t overlay_type, bool do_panic, bool simulation, bool new_entry) {
    return memmap_alloc_range_in(memmap, &memmap_entries, base, length, type, overlay_type, do_panic, simulation, new_entry);
}
