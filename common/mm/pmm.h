#ifndef MM__PMM_H__
#define MM__PMM_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct memmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t slot_count;
};

#define MEMMAP_USABLE                 1
#define MEMMAP_RESERVED               2
#define MEMMAP_ACPI_RECLAIMABLE       3
#define MEMMAP_ACPI_NVS               4
#define MEMMAP_BAD_MEMORY             5
#define MEMMAP_BOOTLOADER_RECLAIMABLE 0x1000
#define MEMMAP_KERNEL_AND_MODULES     0x1001
#define MEMMAP_FRAMEBUFFER            0x1002
#define MEMMAP_RESERVED_MAPPED        0x1003
#define MEMMAP_EFI_RECLAIMABLE        0x2000

struct meminfo {
    size_t uppermem;
    size_t lowermem;
};

struct meminfo mmap_get_info(size_t mmap_count, struct memmap_entry *mmap);

#if defined (BIOS)
extern struct memmap_entry memmap[];
extern size_t memmap_entries;
#endif

#if defined (UEFI)
extern struct memmap_entry *memmap;
extern size_t memmap_entries;

extern struct memmap_entry *untouched_memmap;
extern size_t untouched_memmap_entries;
#endif

extern bool allocations_disallowed;

void init_memmap(void);
struct memmap_entry *get_memmap(size_t *entries);
struct memmap_entry *get_raw_memmap(size_t *entry_count);
void print_memmap(struct memmap_entry *mm, size_t size);
uint64_t pmm_check_type(uint64_t addr);
bool memmap_alloc_range_in(struct memmap_entry *m, size_t *_count,
                           uint64_t base, uint64_t length, uint32_t type, uint32_t overlay_type, bool do_panic, bool simulation, bool new_entry);
bool memmap_alloc_range(uint64_t base, uint64_t length, uint32_t type, uint32_t overlay_type, bool panic, bool simulation, bool new_entry);
void pmm_randomise_memory(void);

void *ext_mem_alloc_size_t(size_t count);
void *ext_mem_alloc(uint64_t count);
void *ext_mem_alloc_counted(uint64_t count, uint64_t elem_size);
void *ext_mem_alloc_type(uint64_t count, uint32_t type);
void *ext_mem_alloc_type_aligned(uint64_t count, uint32_t type, size_t alignment);
void *ext_mem_alloc_type_aligned_mode(uint64_t count, uint32_t type, size_t alignment, bool allow_high_allocs);
void *ext_mem_alloc_type_aligned_mode_random(uint64_t count, uint32_t type, size_t alignment, bool allow_high_allocs, bool random);

void *conv_mem_alloc(uint64_t count);

void pmm_free_size_t(void *ptr, size_t length);
void pmm_free(void *ptr, uint64_t length);
void *pmm_realloc(void *old_ptr, uint64_t old_size, uint64_t new_size);

#if defined (UEFI)
void pmm_release_uefi_mem(void);
#endif

bool check_usable_memory(uint64_t base, uint64_t top);
void pmm_sanitise_entries(struct memmap_entry *m, size_t *_count, bool align_entries);

extern bool pmm_sanitiser_keep_first_page;

#endif
