#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdnoreturn.h>
#include <config.h>
#include <lib/elf.h>
#include <lib/misc.h>
#include <lib/acpi.h>
#include <lib/config.h>
#include <lib/time.h>
#include <lib/pe.h>
#include <lib/print.h>
#include <lib/real.h>
#include <lib/libc.h>
#include <lib/gterm.h>
#include <lib/fdt.h>
#include <libfdt/libfdt.h>
#include <lib/uri.h>
#include <sys/smp.h>
#include <sys/cpu.h>
#include <sys/gdt.h>
#include <lib/fb.h>
#include <lib/term.h>
#include <flanterm/backends/fb.h>
#include <sys/pic.h>
#include <sys/lapic.h>
#include <sys/idt.h>
#include <fs/file.h>
#include <mm/pmm.h>
#include <pxe/tftp.h>
#include <drivers/edid.h>
#include <drivers/vga_textmode.h>
#include <lib/rand.h>
#define LIMINE_NO_POINTERS
#include <protos/limine.h>
#include <limine.h>

enum executable_format {
    EXECUTABLE_FORMAT_ELF,
    EXECUTABLE_FORMAT_PE,
};

static enum executable_format detect_kernel_format(uint8_t *kernel) {
    if (elf_bits(kernel) != -1) {
        return EXECUTABLE_FORMAT_ELF;
    } else if (pe_bits(kernel) != -1) {
        return EXECUTABLE_FORMAT_PE;
    } else {
        panic(true, "limine: Unknown kernel executable format");
    }
}

#define SUPPORTED_BASE_REVISION 3

#define MAX_REQUESTS 128

#define MEMMAP_MAX 1024

static int paging_mode;

static uint64_t get_hhdm_span_top(int base_revision) {
    uint64_t ret = base_revision >= 3 ? 0 : 0x100000000;
    for (size_t i = 0; i < memmap_entries; i++) {
        if (base_revision >= 1 && base_revision < 3 && (
            memmap[i].type == MEMMAP_RESERVED
         || memmap[i].type == MEMMAP_BAD_MEMORY)) {
            continue;
        }

        if (base_revision >= 3 && (
            memmap[i].type != MEMMAP_USABLE
         && memmap[i].type != MEMMAP_BOOTLOADER_RECLAIMABLE
         && memmap[i].type != MEMMAP_KERNEL_AND_MODULES
         && memmap[i].type != MEMMAP_FRAMEBUFFER
         && memmap[i].type != MEMMAP_EFI_RECLAIMABLE)) {
            continue;
        }

        uint64_t base = memmap[i].base;
        uint64_t length = memmap[i].length;
        uint64_t top = base + length;

        if (base_revision < 3 && base < 0x100000000) {
            base = 0x100000000;
        }

        if (base >= top) {
            continue;
        }

        uint64_t aligned_top = ALIGN_UP(top, 0x40000000);

        if (aligned_top > ret) {
            ret = aligned_top;
        }
    }

    return ret;
}

#if defined (__i386__)
static pagemap_t build_identity_map(void) {
    pagemap_t pagemap = new_pagemap(paging_mode);

    map_pages(pagemap, 0, 0, VMM_FLAG_WRITE, 0x100000000);

    size_t _memmap_entries = memmap_entries;
    struct memmap_entry *_memmap =
        ext_mem_alloc(_memmap_entries * sizeof(struct memmap_entry));
    for (size_t i = 0; i < _memmap_entries; i++) {
        _memmap[i] = memmap[i];
    }

    for (size_t i = 0; i < _memmap_entries; i++) {
        if (_memmap[i].type != MEMMAP_USABLE
         && _memmap[i].type != MEMMAP_BOOTLOADER_RECLAIMABLE
         && _memmap[i].type != MEMMAP_KERNEL_AND_MODULES
         && _memmap[i].type != MEMMAP_FRAMEBUFFER
         && _memmap[i].type != MEMMAP_EFI_RECLAIMABLE) {
            continue;
        }

        uint64_t base   = _memmap[i].base;
        uint64_t length = _memmap[i].length;
        uint64_t top    = base + length;

        if (base < 0x100000000) {
            base = 0x100000000;
        }

        if (base >= top) {
            continue;
        }

        uint64_t aligned_base   = ALIGN_DOWN(base, 0x1000);
        uint64_t aligned_top    = ALIGN_UP(top, 0x1000);
        uint64_t aligned_length = aligned_top - aligned_base;

        map_pages(pagemap, aligned_base, aligned_base, VMM_FLAG_WRITE, aligned_length);
    }

    return pagemap;
}

void limine_memcpy_to_64_asm(int paging_mode, void *pagemap, uint64_t dst, void *src, size_t count);

static void limine_memcpy_to_64(uint64_t dst, void *src, size_t count) {
    static bool identity_map_ready = false;
    static pagemap_t identity_map;

    if (!identity_map_ready) {
        identity_map = build_identity_map();
        identity_map_ready = true;
    }

    limine_memcpy_to_64_asm(paging_mode, identity_map.top_level, dst, src, count);
}
#endif

static pagemap_t build_pagemap(int base_revision,
                               bool nx, struct mem_range *ranges, size_t ranges_count,
                               uint64_t physical_base, uint64_t virtual_base,
                               uint64_t direct_map_offset) {
    pagemap_t pagemap = new_pagemap(paging_mode);

    if (ranges_count == 0) {
        panic(true, "limine: ranges_count == 0");
    }

    for (size_t i = 0; i < ranges_count; i++) {
        uint64_t virt = ranges[i].base;
        uint64_t phys;

        if (virt & ((uint64_t)1 << 63)) {
            phys = physical_base + (virt - virtual_base);
        } else {
            panic(false, "limine: Virtual address of a PHDR in lower half");
        }

        uint64_t pf =
            (ranges[i].permissions & MEM_RANGE_X ? 0 : (nx ? VMM_FLAG_NOEXEC : 0)) |
            (ranges[i].permissions & MEM_RANGE_W ? VMM_FLAG_WRITE : 0);

        map_pages(pagemap, virt, phys, pf, ranges[i].length);
    }

    // Map 0x1000->4GiB range to identity if base revision == 0
    if (base_revision == 0) {
        map_pages(pagemap, 0x1000, 0x1000, VMM_FLAG_WRITE, 0x100000000 - 0x1000);
    }

    // Map 0->4GiB range to HHDM if base revision < 3
    if (base_revision < 3) {
        map_pages(pagemap, direct_map_offset, 0, VMM_FLAG_WRITE, 0x100000000);
    }

    size_t _memmap_entries = memmap_entries;
    struct memmap_entry *_memmap =
        ext_mem_alloc(_memmap_entries * sizeof(struct memmap_entry));
    for (size_t i = 0; i < _memmap_entries; i++)
        _memmap[i] = memmap[i];

    // Map all free memory regions to the higher half direct map offset
    for (size_t i = 0; i < _memmap_entries; i++) {
        if (base_revision >= 1 && base_revision < 3 && (
            _memmap[i].type == MEMMAP_RESERVED
         || _memmap[i].type == MEMMAP_BAD_MEMORY)) {
            continue;
        }

        if (base_revision >= 3 && (
            _memmap[i].type != MEMMAP_USABLE
         && _memmap[i].type != MEMMAP_BOOTLOADER_RECLAIMABLE
         && _memmap[i].type != MEMMAP_KERNEL_AND_MODULES
         && _memmap[i].type != MEMMAP_FRAMEBUFFER
         && _memmap[i].type != MEMMAP_EFI_RECLAIMABLE)) {
            continue;
        }

        uint64_t base   = _memmap[i].base;
        uint64_t length = _memmap[i].length;
        uint64_t top    = base + length;

        if (base_revision < 3 && base < 0x100000000) {
            base = 0x100000000;
        }

        if (base >= top) {
            continue;
        }

        uint64_t aligned_base   = ALIGN_DOWN(base, 0x1000);
        uint64_t aligned_top    = ALIGN_UP(top, 0x1000);
        uint64_t aligned_length = aligned_top - aligned_base;

        if (base_revision == 0) {
            map_pages(pagemap, aligned_base, aligned_base, VMM_FLAG_WRITE, aligned_length);
        }
        map_pages(pagemap, direct_map_offset + aligned_base, aligned_base, VMM_FLAG_WRITE, aligned_length);
    }

    // Map the framebuffer with appropriate permissions
    for (size_t i = 0; i < _memmap_entries; i++) {
        if (_memmap[i].type != MEMMAP_FRAMEBUFFER) {
            continue;
        }

        uint64_t base   = _memmap[i].base;
        uint64_t length = _memmap[i].length;
        uint64_t top    = base + length;

        uint64_t aligned_base   = ALIGN_DOWN(base, 0x1000);
        uint64_t aligned_top    = ALIGN_UP(top, 0x1000);
        uint64_t aligned_length = aligned_top - aligned_base;

        if (base_revision == 0) {
            map_pages(pagemap, aligned_base, aligned_base, VMM_FLAG_WRITE | VMM_FLAG_FB, aligned_length);
        }
        map_pages(pagemap, direct_map_offset + aligned_base, aligned_base, VMM_FLAG_WRITE | VMM_FLAG_FB, aligned_length);
    }

    // XXX we do this as a quick and dirty way to switch to the higher half
#if defined (__x86_64__) || defined (__i386__)
    if (base_revision >= 1) {
        map_pages(pagemap, 0, 0, VMM_FLAG_WRITE, 0x100000000);
    }
#endif

    return pagemap;
}

#if defined (__x86_64__) || defined (__i386__)
extern symbol limine_spinup_32;
#elif defined (__aarch64__)

#define LIMINE_SCTLR ((1 << 29) /* Res1 */                \
                    | (1 << 28) /* Res1 */                \
                    | (1 << 23) /* Res1 */                \
                    | (1 << 22) /* Res1 */                \
                    | (1 << 20) /* Res1 */                \
                    | (1 << 12) /* I-Cache */             \
                    | (1 << 11) /* Res1 */                \
                    | (1 << 8)  /* Res1 */                \
                    | (1 << 7)  /* Res1 */                \
                    | (1 << 4)  /* SP0 Alignment check */ \
                    | (1 << 3)  /* SP Alignment check */  \
                    | (1 << 2)  /* D-Cache */             \
                    | (1 << 0)) /* MMU */                 \

#define LIMINE_MAIR(fb) ( ((uint64_t)0b11111111 << 0) /* Normal WB RW-allocate non-transient */ \
                        | ((uint64_t)(fb) << 8) )     /* Framebuffer type */

#define LIMINE_TCR(tsz, pa) ( ((uint64_t)(pa) << 32)         /* Intermediate address size */  \
                            | ((uint64_t)2 << 30)            /* TTBR1 4K granule */           \
                            | ((uint64_t)2 << 28)            /* TTBR1 Inner shareable */      \
                            | ((uint64_t)1 << 26)            /* TTBR1 Outer WB RW-Allocate */ \
                            | ((uint64_t)1 << 24)            /* TTBR1 Inner WB RW-Allocate */ \
                            | ((uint64_t)(tsz) << 16)        /* Address bits in TTBR1 */      \
                                                             /* TTBR0 4K granule */           \
                            | ((uint64_t)2 << 12)            /* TTBR0 Inner shareable */      \
                            | ((uint64_t)1 << 10)            /* TTBR0 Outer WB RW-Allocate */ \
                            | ((uint64_t)1 << 8)             /* TTBR0 Inner WB RW-Allocate */ \
                            | ((uint64_t)(tsz) << 0))        /* Address bits in TTBR0 */

#elif defined (__riscv)
#elif defined (__loongarch64)
#else
#error Unknown architecture
#endif

static uint64_t physical_base, virtual_base, slide, direct_map_offset;
static size_t requests_count;
static void **requests;

static void set_paging_mode(bool randomise_hhdm_base) {
    direct_map_offset = paging_mode_higher_half(paging_mode);
    if (randomise_hhdm_base) {
        // A quarter of the higher half of wiggle room for KASLR, align to 1GiB steps.
        uint64_t mask = ((uint64_t)1 << (paging_mode_va_bits(paging_mode) - 3)) - 1;
        direct_map_offset += (rand64() & ~((uint64_t)0x40000000 - 1)) & mask;
    }
}

static uint64_t reported_addr(void *addr) {
    return (uint64_t)(uintptr_t)addr + direct_map_offset;
}

#if defined (__i386__)
static uint64_t reported_addr_64(uint64_t addr) {
    return addr + direct_map_offset;
}
#endif

#define get_phys_addr(addr) ({ \
    __auto_type get_phys_addr__addr = (addr); \
    uintptr_t get_phys_addr__r; \
    if (get_phys_addr__addr & ((uint64_t)1 << 63)) { \
        get_phys_addr__r = physical_base + (get_phys_addr__addr - virtual_base); \
    } else { \
        get_phys_addr__r = get_phys_addr__addr; \
    } \
    get_phys_addr__r; \
})

static struct limine_file get_file(struct file_handle *file, char *cmdline, bool kernel) {
    struct limine_file ret = {0};

    if (file->pxe) {
        ret.media_type = LIMINE_MEDIA_TYPE_TFTP;

        ret.tftp_ip = file->pxe_ip;
        ret.tftp_port = file->pxe_port;
    } else {
        struct volume *vol = file->vol;

        if (vol->is_optical) {
            ret.media_type = LIMINE_MEDIA_TYPE_OPTICAL;
        }

        ret.partition_index = vol->partition;

        ret.mbr_disk_id = mbr_get_id(vol);

        if (vol->guid_valid) {
            memcpy(&ret.part_uuid, &vol->guid, sizeof(struct limine_uuid));
        }

        if (vol->part_guid_valid) {
            memcpy(&ret.gpt_part_uuid, &vol->part_guid, sizeof(struct limine_uuid));
        }

        struct guid gpt_disk_uuid;
        if (gpt_get_guid(&gpt_disk_uuid, vol->backing_dev ?: vol) == true) {
            memcpy(&ret.gpt_disk_uuid, &gpt_disk_uuid, sizeof(struct limine_uuid));
        }
    }

    char *path = ext_mem_alloc(file->path_len);
    memcpy(path, file->path, file->path_len);

    ret.path = reported_addr(path);

    void *freadall_ret = freadall_mode(file, MEMMAP_KERNEL_AND_MODULES, !kernel
#if defined (__i386__)
        , limine_memcpy_to_64
#endif
    );
#if defined (__i386__)
    ret.address = kernel ? reported_addr(freadall_ret) : reported_addr_64(*(uint64_t *)freadall_ret);
#else
    ret.address = reported_addr(freadall_ret);
#endif

    ret.size = file->size;

    ret.cmdline = reported_addr(cmdline);

    return ret;
}

static void *_get_request(uint64_t id[4]) {
    for (size_t i = 0; i < requests_count; i++) {
        uint64_t *p = requests[i];

        if (p[2] != id[2]) {
            continue;
        }
        if (p[3] != id[3]) {
            continue;
        }

        return p;
    }

    return NULL;
}

#define get_request(REQ) _get_request((uint64_t[4])REQ)

#define FEAT_START do {
#define FEAT_END } while (0);

noreturn void limine_load(char *config, char *cmdline) {
#if defined (__x86_64__) || defined (__i386__)
    uint32_t eax, ebx, ecx, edx;
#endif

    char *kernel_path = config_get_value(config, 0, "PATH");
    if (kernel_path == NULL) {
        kernel_path = config_get_value(config, 0, "KERNEL_PATH");
    }
    if (kernel_path == NULL) {
        panic(true, "limine: Executable path not specified");
    }

    print("limine: Loading executable `%#`...\n", kernel_path);

    struct file_handle *kernel_file;
    if ((kernel_file = uri_open(kernel_path)) == NULL)
        panic(true, "limine: Failed to open executable with path `%#`. Is the path correct?", kernel_path);

    char *k_path_copy = ext_mem_alloc(strlen(kernel_path) + 1);
    strcpy(k_path_copy, kernel_path);
    char *k_resource = NULL, *k_root = NULL, *k_path = NULL, *k_hash = NULL;
    uri_resolve(k_path_copy, &k_resource, &k_root, &k_path, &k_hash);
    char *k_path_ = ext_mem_alloc(strlen(k_path) + 2);
    k_path_[0] = '/';
    strcpy(k_path_ + 1, k_path);
    k_path = k_path_;
    for (size_t i = strlen(k_path) - 1; ; i--) {
        if (k_path[i] == '/' || i == 1) {
            k_path[i] = 0;
            break;
        }
        k_path[i] = 0;
    }

    uint8_t *kernel = freadall(kernel_file, MEMMAP_BOOTLOADER_RECLAIMABLE);

    char *kaslr_s = config_get_value(config, 0, "KASLR");
    bool kaslr = true;
    if (kaslr_s != NULL && strcmp(kaslr_s, "no") == 0)
        kaslr = false;

    // ELF loading
    uint64_t entry_point = 0;
    struct mem_range *ranges;
    uint64_t ranges_count;

    uint64_t image_size_before_bss;
    bool is_reloc;

    enum executable_format kernel_format = detect_kernel_format(kernel);
    switch (kernel_format) {
        case EXECUTABLE_FORMAT_ELF:
            if (!elf64_load(kernel, &entry_point, &slide,
                            MEMMAP_KERNEL_AND_MODULES, kaslr,
                            &ranges, &ranges_count,
                            &physical_base, &virtual_base, NULL,
                            &image_size_before_bss,
                            &is_reloc)) {
                panic(true, "limine: ELF64 load failure");
            }
            break;
        case EXECUTABLE_FORMAT_PE:
            if (!pe64_load(kernel, &entry_point, &slide,
                            MEMMAP_KERNEL_AND_MODULES, kaslr,
                            &ranges, &ranges_count,
                            &physical_base, &virtual_base, NULL,
                            &image_size_before_bss,
                            &is_reloc)) {
                panic(true, "limine: PE64 load failure");
            }
            break;
    }

    kaslr = kaslr && is_reloc;

    LIMINE_REQUESTS_START_MARKER;
    LIMINE_REQUESTS_END_MARKER;

    // Determine base revision
    LIMINE_BASE_REVISION(0);
    int base_revision = 0;
    uint64_t *base_rev_p1_ptr = NULL;
    uint64_t *base_rev_p2_ptr = NULL;
    for (size_t i = 0; i < ALIGN_DOWN(image_size_before_bss, 8); i += 8) {
        uint64_t *p = (void *)(uintptr_t)physical_base + i;

        // Check if start marker hit
        if (p[0] == limine_requests_start_marker[0] && p[1] == limine_requests_start_marker[1]
         && p[2] == limine_requests_start_marker[2] && p[3] == limine_requests_start_marker[3]) {
            base_revision = 0;
            base_rev_p1_ptr = NULL;
            base_rev_p2_ptr = NULL;
            continue;
        }

        // Check if end marker hit
        if (p[0] == limine_requests_end_marker[0] && p[1] == limine_requests_end_marker[1]) {
            break;
        }

        if (p[0] == limine_base_revision[0] && p[1] == limine_base_revision[1]) {
            if (base_revision != 0) {
                panic(true, "limine: Duplicated base revision tag");
            }
            base_revision = p[2];
            if (p[2] <= SUPPORTED_BASE_REVISION) {
                // Set to 0 to mean "supported"
                base_rev_p2_ptr = &p[2];
            } else {
                base_revision = SUPPORTED_BASE_REVISION;
            }
            base_rev_p1_ptr = &p[1];
        }
    }
    if (base_rev_p1_ptr != NULL) {
        *base_rev_p1_ptr = base_revision;
    }
    if (base_rev_p2_ptr != NULL) {
        *base_rev_p2_ptr = 0;
    }

    // Load requests
    uint64_t *limine_reqs = NULL;
    requests = ext_mem_alloc(MAX_REQUESTS * sizeof(void *));
    requests_count = 0;
    if (base_revision == 0 && kernel_format == EXECUTABLE_FORMAT_ELF && elf64_load_section(kernel, &limine_reqs, ".limine_reqs", 0, slide)) {
        for (size_t i = 0; ; i++) {
            if (limine_reqs[i] == 0) {
                break;
            }
            requests[i] = (void *)(uintptr_t)((limine_reqs[i] - virtual_base) + physical_base);
            requests_count++;
        }
    } else {
        uint64_t common_magic[2] = { LIMINE_COMMON_MAGIC };
        for (size_t i = 0; i < ALIGN_DOWN(image_size_before_bss, 8); i += 8) {
            uint64_t *p = (void *)(uintptr_t)physical_base + i;

            // Check if start marker hit
            if (p[0] == limine_requests_start_marker[0] && p[1] == limine_requests_start_marker[1]
             && p[2] == limine_requests_start_marker[2] && p[3] == limine_requests_start_marker[3]) {
                requests_count = 0;
                continue;
            }

            // Check if end marker hit
            if (p[0] == limine_requests_end_marker[0] && p[1] == limine_requests_end_marker[1]) {
                break;
            }

            if (p[0] != common_magic[0]) {
                continue;
            }
            if (p[1] != common_magic[1]) {
                continue;
            }

            if (requests_count == MAX_REQUESTS) {
                panic(true, "limine: Maximum requests exceeded");
            }

            // Check for a conflict
            if (_get_request(p) != NULL) {
                panic(true, "limine: Conflict detected for request ID %X %X", p[2], p[3]);
            }

            requests[requests_count++] = p;
        }
    }

#if defined (__x86_64__) || defined (__i386__)
    // Check if 64 bit CPU
    if (!cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx) || !(edx & (1 << 29))) {
        panic(true, "limine: This CPU does not support 64-bit mode.");
    }
#endif

    uint64_t hhdm_span_top = get_hhdm_span_top(base_revision);

#if defined (__x86_64__) || defined (__i386__)
    uint64_t maxphyaddr;
    if (!cpuid(0x80000008, 0, &eax, &ebx, &ecx, &edx)) {
        maxphyaddr = 36;
    } else {
        maxphyaddr = eax & 0xff;
    }

    if (maxphyaddr > 64) {
        panic(true, "limine: MAXPHYADDR > 64");
    }
    if (maxphyaddr < 64 && hhdm_span_top > (uint64_t)1 << maxphyaddr) {
        panic(true, "limine: Top of HHDM exceeds maximum allowable MAXPHYADDR value");
    }
#endif

    printv("limine: Physical base:   %X\n", physical_base);
    printv("limine: Virtual base:    %X\n", virtual_base);
    printv("limine: Slide:           %X\n", slide);
    printv("limine: ELF entry point: %X\n", entry_point);
    printv("limine: Base revision:   %u\n", base_revision);
    printv("limine: Requests count:  %u\n", requests_count);
    printv("limine: Top of HHDM:     %X\n", hhdm_span_top);

    // Paging Mode
    int max_supported_paging_mode, min_supported_paging_mode;

#if defined (__x86_64__) || defined (__i386__)
    max_supported_paging_mode = PAGING_MODE_X86_64_4LVL;
    if (cpuid(0x00000007, 0, &eax, &ebx, &ecx, &edx) && (ecx & (1 << 16))) {
        printv("limine: CPU has 5-level paging support\n");
        max_supported_paging_mode = PAGING_MODE_X86_64_5LVL;
    }
    min_supported_paging_mode = PAGING_MODE_X86_64_4LVL;
    if (hhdm_span_top >= (uint64_t)1 << (paging_mode_va_bits(min_supported_paging_mode) - 2)) {
        min_supported_paging_mode = PAGING_MODE_X86_64_5LVL;
        if (min_supported_paging_mode > max_supported_paging_mode) {
            goto hhdm_fail;
        }
    }
    if (hhdm_span_top >= (uint64_t)1 << (paging_mode_va_bits(min_supported_paging_mode) - 2)) {
        goto hhdm_fail;
    }
#elif defined (__aarch64__)
    max_supported_paging_mode = PAGING_MODE_AARCH64_4LVL;
    min_supported_paging_mode = PAGING_MODE_AARCH64_4LVL;
    if (hhdm_span_top >= (uint64_t)1 << (paging_mode_va_bits(min_supported_paging_mode) - 2)) {
        goto hhdm_fail;
    }
    // TODO(qookie): aarch64 also has optional 5 level paging when using 4K pages
#elif defined (__riscv)
    max_supported_paging_mode = vmm_max_paging_mode();
    min_supported_paging_mode = PAGING_MODE_RISCV_SV39;
    if (hhdm_span_top >= (uint64_t)1 << (paging_mode_va_bits(min_supported_paging_mode) - 2)) {
        min_supported_paging_mode = PAGING_MODE_RISCV_SV48;
        if (min_supported_paging_mode > max_supported_paging_mode) {
            goto hhdm_fail;
        }
    }
    if (hhdm_span_top >= (uint64_t)1 << (paging_mode_va_bits(min_supported_paging_mode) - 2)) {
        min_supported_paging_mode = PAGING_MODE_RISCV_SV57;
        if (min_supported_paging_mode > max_supported_paging_mode) {
            goto hhdm_fail;
        }
    }
    if (hhdm_span_top >= (uint64_t)1 << (paging_mode_va_bits(min_supported_paging_mode) - 2)) {
        goto hhdm_fail;
    }
#elif defined (__loongarch64)
    max_supported_paging_mode = PAGING_MODE_LOONGARCH64_4LVL;
    min_supported_paging_mode = PAGING_MODE_LOONGARCH64_4LVL;
    if (hhdm_span_top >= (uint64_t)1 << (paging_mode_va_bits(min_supported_paging_mode) - 2)) {
        goto hhdm_fail;
    }
#else
#error Unknown architecture
#endif

    if (0) {
hhdm_fail:
        panic(true, "limine: Unable to allocate higher half direct map (too much memory?)");
    }

    char *user_paging_mode_s = config_get_value(config, 0, "PAGING_MODE");

    int user_max_paging_mode = PAGING_MODE_MAX;

    char *user_max_paging_mode_s;
    if (user_paging_mode_s != NULL) {
        user_max_paging_mode_s = user_paging_mode_s;
    } else {
        user_max_paging_mode_s = config_get_value(config, 0, "MAX_PAGING_MODE");
    }
    if (user_max_paging_mode_s != NULL) {
#if defined (__x86_64__) || defined (__i386__)
        if (strcasecmp(user_max_paging_mode_s, "4level") == 0) {
            user_max_paging_mode = PAGING_MODE_X86_64_4LVL;
        } else if (strcasecmp(user_max_paging_mode_s, "5level") == 0) {
            user_max_paging_mode = PAGING_MODE_X86_64_5LVL;
        }
#elif defined (__aarch64__)
        if (strcasecmp(user_max_paging_mode_s, "4level") == 0) {
            user_max_paging_mode = PAGING_MODE_AARCH64_4LVL;
        } else if (strcasecmp(user_max_paging_mode_s, "5level") == 0) {
            user_max_paging_mode = PAGING_MODE_AARCH64_5LVL;
        }
#elif defined (__riscv)
        if (strcasecmp(user_max_paging_mode_s, "sv39") == 0) {
            user_max_paging_mode = PAGING_MODE_RISCV_SV39;
        } else if (strcasecmp(user_max_paging_mode_s, "sv48") == 0) {
            user_max_paging_mode = PAGING_MODE_RISCV_SV48;
        } else if (strcasecmp(user_max_paging_mode_s, "sv57") == 0) {
            user_max_paging_mode = PAGING_MODE_RISCV_SV57;
        }
#elif defined (__loongarch64)
        if (strcasecmp(user_max_paging_mode_s, "4level") == 0) {
            user_max_paging_mode = PAGING_MODE_LOONGARCH64_4LVL;
        }
#endif
        else {
            panic(true, "limine: Invalid MAX_PAGING_MODE: `%s`", user_max_paging_mode_s);
        }
    }

    int user_min_paging_mode = PAGING_MODE_MIN;

    char *user_min_paging_mode_s;
    if (user_paging_mode_s != NULL) {
        user_min_paging_mode_s = user_paging_mode_s;
    } else {
        user_min_paging_mode_s = config_get_value(config, 0, "MIN_PAGING_MODE");
    }
    if (user_min_paging_mode_s != NULL) {
#if defined (__x86_64__) || defined (__i386__)
        if (strcasecmp(user_min_paging_mode_s, "4level") == 0) {
            user_min_paging_mode = PAGING_MODE_X86_64_4LVL;
        } else if (strcasecmp(user_min_paging_mode_s, "5level") == 0) {
            user_min_paging_mode = PAGING_MODE_X86_64_5LVL;
        }
#elif defined (__aarch64__)
        if (strcasecmp(user_min_paging_mode_s, "4level") == 0) {
            user_min_paging_mode = PAGING_MODE_AARCH64_4LVL;
        } else if (strcasecmp(user_min_paging_mode_s, "5level") == 0) {
            user_min_paging_mode = PAGING_MODE_AARCH64_5LVL;
        }
#elif defined (__riscv)
        if (strcasecmp(user_min_paging_mode_s, "sv39") == 0) {
            user_min_paging_mode = PAGING_MODE_RISCV_SV39;
        } else if (strcasecmp(user_min_paging_mode_s, "sv48") == 0) {
            user_min_paging_mode = PAGING_MODE_RISCV_SV48;
        } else if (strcasecmp(user_min_paging_mode_s, "sv57") == 0) {
            user_min_paging_mode = PAGING_MODE_RISCV_SV57;
        }
#elif defined (__loongarch64)
        if (strcasecmp(user_min_paging_mode_s, "4level") == 0) {
            user_min_paging_mode = PAGING_MODE_LOONGARCH64_4LVL;
        }
#endif
        else {
            panic(true, "limine: Invalid MIN_PAGING_MODE: `%s`", user_min_paging_mode_s);
        }
    }

    if (user_max_paging_mode < user_min_paging_mode) {
        panic(true, "limine: MAX_PAGING_MODE is lower than MIN_PAGING_MODE");
    }

    if (user_max_paging_mode < max_supported_paging_mode) {
        if (user_max_paging_mode < min_supported_paging_mode) {
            panic(true, "limine: User set MAX_PAGING_MODE less than minimum supported paging mode");
        }
        max_supported_paging_mode = user_max_paging_mode;
    }
    if (user_min_paging_mode > min_supported_paging_mode) {
        if (user_min_paging_mode > max_supported_paging_mode) {
            panic(true, "limine: User set MIN_PAGING_MODE greater than maximum supported paging mode");
        }
        min_supported_paging_mode = user_min_paging_mode;
    }

#if defined (__x86_64__) || defined (__i386__)
    paging_mode = PAGING_MODE_X86_64_4LVL;
#elif defined (__riscv)
    paging_mode = max_supported_paging_mode >= PAGING_MODE_RISCV_SV48 ? PAGING_MODE_RISCV_SV48 : PAGING_MODE_RISCV_SV39;
#elif defined (__aarch64__)
    paging_mode = PAGING_MODE_AARCH64_4LVL;
#elif defined (__loongarch64)
    paging_mode = PAGING_MODE_LOONGARCH64_4LVL;
#endif

#if defined (__riscv)
#define paging_mode_limine_to_vmm(x) (PAGING_MODE_RISCV_SV39 + (x))
#define paging_mode_vmm_to_limine(x) ((x) - PAGING_MODE_RISCV_SV39)
#else
#define paging_mode_limine_to_vmm(x) (x)
#define paging_mode_vmm_to_limine(x) (x)
#endif

    bool paging_mode_set = false;
    bool randomise_hhdm_base = false;
FEAT_START
    struct limine_paging_mode_request *pm_request = get_request(LIMINE_PAGING_MODE_REQUEST);
    if (pm_request == NULL)
        break;

    uint64_t target_mode = pm_request->mode;
    paging_mode = paging_mode_limine_to_vmm(target_mode);

    int kern_min_mode = PAGING_MODE_MIN, kern_max_mode = paging_mode;
    if (pm_request->revision >= 1) {
        kern_min_mode = (int)paging_mode_limine_to_vmm(pm_request->min_mode);
        kern_max_mode = (int)paging_mode_limine_to_vmm(pm_request->max_mode);
    }

    if (paging_mode > max_supported_paging_mode) {
        paging_mode = max_supported_paging_mode;
    }
    if (paging_mode < min_supported_paging_mode) {
        paging_mode = min_supported_paging_mode;
    }

    if (kern_max_mode < kern_min_mode) {
        panic(true, "limine: Executable's paging max_mode lower than min_mode");
    }

    if (paging_mode > kern_max_mode) {
        if (kern_max_mode < min_supported_paging_mode) {
            panic(true, "limine: Executable's maximum supported paging mode lower than minimum allowable paging mode");
        }
        paging_mode = kern_max_mode;
    }
    if (paging_mode < kern_min_mode) {
        if (kern_min_mode > max_supported_paging_mode) {
            panic(true, "limine: Executable's minimum supported paging mode higher than maximum allowable paging mode");
        }
        paging_mode = kern_min_mode;
    }

    char *randomise_hhdm_base_s = config_get_value(config, 0, "RANDOMISE_HHDM_BASE");
    if (randomise_hhdm_base_s == NULL) {
        randomise_hhdm_base_s = config_get_value(config, 0, "RANDOMIZE_HHDM_BASE");
    }
    if (randomise_hhdm_base_s == NULL) {
        randomise_hhdm_base = kaslr;
    } else {
        randomise_hhdm_base = strcasecmp(randomise_hhdm_base_s, "no") != 0;
    }

    set_paging_mode(randomise_hhdm_base);
    paging_mode_set = true;

    struct limine_paging_mode_response *pm_response =
        ext_mem_alloc(sizeof(struct limine_paging_mode_response));

    pm_response->mode = paging_mode_vmm_to_limine(paging_mode);
    pm_request->response = reported_addr(pm_response);
FEAT_END

    if (!paging_mode_set) {
        set_paging_mode(randomise_hhdm_base);
    }

#if defined (__aarch64__)
    uint64_t aa64mmfr0;
    asm volatile ("mrs %0, id_aa64mmfr0_el1" : "=r" (aa64mmfr0));

    uint64_t pa = aa64mmfr0 & 0xF;

    uint64_t tsz = 64 - (paging_mode_va_bits(paging_mode) - 1);
#endif

    struct limine_file *kf = ext_mem_alloc(sizeof(struct limine_file));
    *kf = get_file(kernel_file, cmdline, true);
    fclose(kernel_file);

    // Entry point feature
FEAT_START
    struct limine_entry_point_request *entrypoint_request = get_request(LIMINE_ENTRY_POINT_REQUEST);
    if (entrypoint_request == NULL) {
        break;
    }

    entry_point = entrypoint_request->entry;

    printv("limine: Entry point at %X\n", entry_point);

    struct limine_entry_point_response *entrypoint_response =
        ext_mem_alloc(sizeof(struct limine_entry_point_response));

    entrypoint_request->response = reported_addr(entrypoint_response);
FEAT_END

    // Bootloader info feature
FEAT_START
    struct limine_bootloader_info_request *bootloader_info_request = get_request(LIMINE_BOOTLOADER_INFO_REQUEST);
    if (bootloader_info_request == NULL) {
        break; // next feature
    }

    struct limine_bootloader_info_response *bootloader_info_response =
        ext_mem_alloc(sizeof(struct limine_bootloader_info_response));

    bootloader_info_response->name = reported_addr("Limine");
    bootloader_info_response->version = reported_addr(LIMINE_VERSION);

    bootloader_info_request->response = reported_addr(bootloader_info_response);
FEAT_END

    // Firmware type feature
FEAT_START
    struct limine_firmware_type_request *firmware_type_request = get_request(LIMINE_FIRMWARE_TYPE_REQUEST);
    if (firmware_type_request == NULL) {
        break; // next feature
    }

    struct limine_firmware_type_response *firmware_type_response =
        ext_mem_alloc(sizeof(struct limine_firmware_type_response));

    firmware_type_response->firmware_type =
#if defined (UEFI)
#if defined (__i386__)
        LIMINE_FIRMWARE_TYPE_UEFI32
#else
        LIMINE_FIRMWARE_TYPE_UEFI64
#endif
#else
        LIMINE_FIRMWARE_TYPE_X86BIOS
#endif
    ;

    firmware_type_request->response = reported_addr(firmware_type_response);
FEAT_END

    // Executable address feature
FEAT_START
    struct limine_executable_address_request *executable_address_request = get_request(LIMINE_EXECUTABLE_ADDRESS_REQUEST);
    if (executable_address_request == NULL) {
        break; // next feature
    }

    struct limine_executable_address_response *executable_address_response =
        ext_mem_alloc(sizeof(struct limine_executable_address_response));

    executable_address_response->physical_base = physical_base;
    executable_address_response->virtual_base = virtual_base;

    executable_address_request->response = reported_addr(executable_address_response);
FEAT_END

    // HHDM feature
FEAT_START
    struct limine_hhdm_request *hhdm_request = get_request(LIMINE_HHDM_REQUEST);
    if (hhdm_request == NULL) {
        break; // next feature
    }

    struct limine_hhdm_response *hhdm_response =
        ext_mem_alloc(sizeof(struct limine_hhdm_response));

    hhdm_response->offset = direct_map_offset;

    hhdm_request->response = reported_addr(hhdm_response);
FEAT_END

    // RSDP feature
FEAT_START
    struct limine_rsdp_request *rsdp_request = get_request(LIMINE_RSDP_REQUEST);
    if (rsdp_request == NULL) {
        break; // next feature
    }

    void *rsdp = acpi_get_rsdp();
    if (rsdp == NULL) {
        break;
    }

    struct limine_rsdp_response *rsdp_response =
        ext_mem_alloc(sizeof(struct limine_rsdp_response));

    rsdp_response->address = base_revision <= 2 ? reported_addr(rsdp) : (uintptr_t)rsdp;

    rsdp_request->response = reported_addr(rsdp_response);
FEAT_END

    // SMBIOS feature
FEAT_START
    struct limine_smbios_request *smbios_request = get_request(LIMINE_SMBIOS_REQUEST);
    if (smbios_request == NULL) {
        break; // next feature
    }

    void *smbios_entry_32 = NULL, *smbios_entry_64 = NULL;
    acpi_get_smbios(&smbios_entry_32, &smbios_entry_64);
    if (smbios_entry_32 == NULL && smbios_entry_64 == NULL) {
        break;
    }

    struct limine_smbios_response *smbios_response =
        ext_mem_alloc(sizeof(struct limine_smbios_response));

    if (smbios_entry_32) {
        smbios_response->entry_32 = base_revision <= 2 ? reported_addr(smbios_entry_32) : (uintptr_t)smbios_entry_32;
    }
    if (smbios_entry_64) {
        smbios_response->entry_64 = base_revision <= 2 ? reported_addr(smbios_entry_64) : (uintptr_t)smbios_entry_64;
    }

    smbios_request->response = reported_addr(smbios_response);
FEAT_END

#if defined (UEFI)
    // EFI system table feature
FEAT_START
    struct limine_efi_system_table_request *est_request = get_request(LIMINE_EFI_SYSTEM_TABLE_REQUEST);
    if (est_request == NULL) {
        break; // next feature
    }

    struct limine_efi_system_table_response *est_response =
        ext_mem_alloc(sizeof(struct limine_efi_system_table_response));

    est_response->address = base_revision <= 2 ? reported_addr(gST) : (uintptr_t)gST;

    est_request->response = reported_addr(est_response);
FEAT_END
#endif

    // Device tree blob feature
FEAT_START
    struct limine_dtb_request *dtb_request = get_request(LIMINE_DTB_REQUEST);
    if (dtb_request == NULL) {
        break; // next feature
    }

    void *dtb = get_device_tree_blob(config, 0);

    if (dtb) {
        // Delete all /memory@... nodes.
        // The executable must use the given UEFI memory map instead.
        while (true) {
            int offset = fdt_subnode_offset_namelen(dtb, 0, "memory@", 7);

            if (offset == -FDT_ERR_NOTFOUND) {
                break;
            }

            if (offset < 0) {
                panic(true, "limine: failed to find node: '%s'", fdt_strerror(offset));
            }

            int ret = fdt_del_node(dtb, offset);
            if (ret < 0) {
                panic(true, "limine: failed to delete memory node: '%s'", fdt_strerror(ret));
            }
        }

        struct limine_dtb_response *dtb_response =
            ext_mem_alloc(sizeof(struct limine_dtb_response));
        dtb_response->dtb_ptr = reported_addr(dtb);
        dtb_request->response = reported_addr(dtb_response);
    }
FEAT_END

    // Stack size
    uint64_t stack_size = 65536;
FEAT_START
    struct limine_stack_size_request *stack_size_request = get_request(LIMINE_STACK_SIZE_REQUEST);
    if (stack_size_request == NULL) {
        break; // next feature
    }

    struct limine_stack_size_response *stack_size_response =
        ext_mem_alloc(sizeof(struct limine_stack_size_response));

    if (stack_size_request->stack_size > stack_size) {
        stack_size = stack_size_request->stack_size;
    }

    stack_size_request->response = reported_addr(stack_size_response);
FEAT_END

    // Executable file
FEAT_START
    struct limine_executable_file_request *executable_file_request = get_request(LIMINE_EXECUTABLE_FILE_REQUEST);
    if (executable_file_request == NULL) {
        break; // next feature
    }

    struct limine_executable_file_response *executable_file_response =
        ext_mem_alloc(sizeof(struct limine_executable_file_response));

    executable_file_response->executable_file = reported_addr(kf);

    executable_file_request->response = reported_addr(executable_file_response);
FEAT_END

    // Modules
FEAT_START
    struct limine_module_request *module_request = get_request(LIMINE_MODULE_REQUEST);
    if (module_request == NULL) {
        break; // next feature
    }

    size_t module_count;
    for (module_count = 0; ; module_count++) {
        char *module_file = config_get_value(config, module_count, "MODULE_PATH");
        if (module_file == NULL)
            break;
    }

    if (module_request->revision >= 1) {
        module_count += module_request->internal_module_count;
    }

    if (module_count == 0) {
        break;
    }

    struct limine_module_response *module_response =
        ext_mem_alloc(sizeof(struct limine_module_response));

    module_response->revision = 2;

    struct limine_file *modules = ext_mem_alloc(module_count * sizeof(struct limine_file));

    size_t final_module_count = 0;
    for (size_t i = 0; i < module_count; i++) {
        char *module_path;
        char *module_cmdline;
        bool module_required = true;
        bool module_path_allocated = false;

        if (module_request->revision >= 1 && i < module_request->internal_module_count) {
            uint64_t *internal_modules = (void *)get_phys_addr(module_request->internal_modules);
            struct limine_internal_module *internal_module = (void *)get_phys_addr(internal_modules[i]);

            module_path = (char *)get_phys_addr(internal_module->path);
            module_cmdline = (char *)get_phys_addr(internal_module->cmdline);

            char *module_path_abs = ext_mem_alloc(1024);
            char *module_path_abs_p = module_path_abs;
            if (internal_module->flags & LIMINE_INTERNAL_MODULE_COMPRESSED) {
                panic(true, "limine: Compressed internal modules no longer supported");
            }
            strcpy(module_path_abs_p, k_resource);
            module_path_abs_p += strlen(k_resource);
            strcpy(module_path_abs_p, (config_format_old ? "://" : "("));
            module_path_abs_p += (config_format_old ? 3 : 1);
            strcpy(module_path_abs_p, k_root);
            module_path_abs_p += strlen(k_root);
            if (!config_format_old) {
                strcpy(module_path_abs_p, "):");
                module_path_abs_p += 2;
            }
            get_absolute_path(module_path_abs_p, module_path, k_path);

            module_path = module_path_abs;
            module_path_allocated = true;

            module_required = internal_module->flags & LIMINE_INTERNAL_MODULE_REQUIRED;
        } else {
            struct conf_tuple conf_tuple =
                    config_get_tuple(config, i - (module_request->revision >= 1 ? module_request->internal_module_count : 0),
                                     "MODULE_PATH", "MODULE_CMDLINE");

            module_path = conf_tuple.value1;
            module_cmdline = conf_tuple.value2;
        }

        if (module_cmdline == NULL) {
            module_cmdline = "";
        }

        print("limine: Loading module `%#`...\n", module_path);

        struct file_handle *f;
        if ((f = uri_open(module_path)) == NULL) {
            if (module_required) {
                panic(true, "limine: Failed to open module with path `%#`. Is the path correct?", module_path);
            }
            printv("limine: Warning: Non-required internal module `%#` not found\n", module_path);
            if (module_path_allocated) {
                pmm_free(module_path, 1024);
            }
            continue;
        }
        if (module_path_allocated) {
            pmm_free(module_path, 1024);
        }

        struct limine_file *l = &modules[final_module_count++];
        *l = get_file(f, module_cmdline, false);

        fclose(f);
    }

    uint64_t *modules_list = ext_mem_alloc(final_module_count * sizeof(uint64_t));
    for (size_t i = 0; i < final_module_count; i++) {
        modules_list[i] = reported_addr(&modules[i]);
    }

    module_response->module_count = final_module_count;
    module_response->modules = reported_addr(modules_list);

    module_request->response = reported_addr(module_response);
FEAT_END

    size_t req_width = 0, req_height = 0, req_bpp = 0;

    char *resolution = config_get_value(config, 0, "RESOLUTION");
    if (resolution != NULL) {
        parse_resolution(&req_width, &req_height, &req_bpp, resolution);
    }

    struct fb_info *fbs;
    size_t fbs_count;

    term_notready();

    fb_init(&fbs, &fbs_count, req_width, req_height, req_bpp);
    if (fbs_count == 0) {
        goto no_fb;
    }

    for (size_t i = 0; i < fbs_count; i++) {
        memmap_alloc_range(fbs[i].framebuffer_addr,
                           (uint64_t)fbs[i].framebuffer_pitch * fbs[i].framebuffer_height,
                           MEMMAP_FRAMEBUFFER, 0, false, false, true);
    }

    // Framebuffer feature
FEAT_START
    struct limine_framebuffer_request *framebuffer_request = get_request(LIMINE_FRAMEBUFFER_REQUEST);
    if (framebuffer_request == NULL) {
        break; // next feature
    }

    if (fbs_count == 0) {
        break;
    }

    struct limine_framebuffer *fbp = ext_mem_alloc(fbs_count * sizeof(struct limine_framebuffer));

    struct limine_framebuffer_response *framebuffer_response =
        ext_mem_alloc(sizeof(struct limine_framebuffer_response));

    framebuffer_response->revision = 1;

    uint64_t *fb_list = ext_mem_alloc(fbs_count * sizeof(uint64_t));

    for (size_t i = 0; i < fbs_count; i++) {
        uint64_t *modes_list = ext_mem_alloc(fbs[i].mode_count * sizeof(uint64_t));
        for (size_t j = 0; j < fbs[i].mode_count; j++) {
            fbs[i].mode_list[j].memory_model = LIMINE_FRAMEBUFFER_RGB;
            modes_list[j] = reported_addr(&fbs[i].mode_list[j]);
        }
        fbp[i].modes = reported_addr(modes_list);
        fbp[i].mode_count = fbs[i].mode_count;

        if (fbs[i].edid != NULL) {
            fbp[i].edid_size = sizeof(struct edid_info_struct);
            fbp[i].edid = reported_addr(fbs[i].edid);
        }

        fbp[i].memory_model     = LIMINE_FRAMEBUFFER_RGB;
        fbp[i].address          = reported_addr((void *)(uintptr_t)fbs[i].framebuffer_addr);
        fbp[i].width            = fbs[i].framebuffer_width;
        fbp[i].height           = fbs[i].framebuffer_height;
        fbp[i].bpp              = fbs[i].framebuffer_bpp;
        fbp[i].pitch            = fbs[i].framebuffer_pitch;
        fbp[i].red_mask_size    = fbs[i].red_mask_size;
        fbp[i].red_mask_shift   = fbs[i].red_mask_shift;
        fbp[i].green_mask_size  = fbs[i].green_mask_size;
        fbp[i].green_mask_shift = fbs[i].green_mask_shift;
        fbp[i].blue_mask_size   = fbs[i].blue_mask_size;
        fbp[i].blue_mask_shift  = fbs[i].blue_mask_shift;

        fb_list[i] = reported_addr(&fbp[i]);
    }

    framebuffer_response->framebuffer_count = fbs_count;
    framebuffer_response->framebuffers = reported_addr(fb_list);

    framebuffer_request->response = reported_addr(framebuffer_response);
FEAT_END

no_fb:
    // Boot time feature
FEAT_START
    struct limine_boot_time_request *boot_time_request = get_request(LIMINE_BOOT_TIME_REQUEST);
    if (boot_time_request == NULL) {
        break; // next feature
    }

    struct limine_boot_time_response *boot_time_response =
        ext_mem_alloc(sizeof(struct limine_boot_time_response));

    boot_time_response->boot_time = time();

    boot_time_request->response = reported_addr(boot_time_response);
FEAT_END

    // Wrap-up stuff before memmap close
#if defined (__x86_64__) || defined (__i386__)
    struct gdtr *local_gdt = ext_mem_alloc(sizeof(struct gdtr));
    local_gdt->limit = gdt.limit;
    uint64_t local_gdt_base = (uint64_t)gdt.ptr;
    local_gdt_base += direct_map_offset;
    local_gdt->ptr = local_gdt_base;
#if defined (__i386__)
    local_gdt->ptr_hi = local_gdt_base >> 32;
#endif
#endif

#if defined (__aarch64__)
    // Find the most restrictive caching mode from all framebuffers to use
    uint64_t fb_attr = (uint64_t)-1;

    for (size_t i = 0; i < fbs_count; i++) {
        int el = current_el();
        uint64_t res;

        // Figure out the caching mode used for this particular framebuffer
        if (el == 1) {
            asm volatile (
                    "at s1e1w, %1\n\t"
                    "isb\n\t"
                    "mrs %0, par_el1"
                    : "=r"(res)
                    : "r"(fbs[i].framebuffer_addr)
                    : "memory");
        } else if (el == 2) {
            asm volatile (
                    "at s1e2w, %1\n\t"
                    "isb\n\t"
                    "mrs %0, par_el1"
                    : "=r"(res)
                    : "r"(fbs[i].framebuffer_addr)
                    : "memory");
        } else {
            panic(false, "Unexpected EL in limine_load");
        }

        if (res & 1)
            panic(false, "Address translation for framebuffer failed");

        uint64_t new_attr = res >> 56;

        // Use whatever we find first
        if (fb_attr == (uint64_t)-1)
            fb_attr = new_attr;
        // Prefer Device memory over Normal memory
        else if ((fb_attr & 0b11110000) && !(new_attr & 0b11110000))
            fb_attr = new_attr;
        // Prefer tighter Device memory (lower values)
        else if (!(fb_attr & 0b11110000) && !(new_attr & 0b11110000) && fb_attr > new_attr)
            fb_attr = new_attr;
        // Use Normal non-cacheable otherwise (avoid trying to figure out how to downgrade inner vs outer).
        else if ((fb_attr & 0b11110000) && (new_attr & 0b11110000))
            fb_attr = 0b01000100; // Inner&outer Non-cacheable
        // Otherwise do nothing (fb_attr is already more restrictive than new_attr).
    }

    // If no framebuffers are found, just zero out the MAIR entry
    if (fb_attr == (uint64_t)-1)
        fb_attr = 0;
#endif

    void *stack = ext_mem_alloc(stack_size) + stack_size;

    bool nx_available = true;
#if defined (__x86_64__) || defined (__i386__)
    // Check if we have NX
    if (!cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx) || !(edx & (1 << 20))) {
        nx_available = false;
    }
#endif

#if defined (UEFI)
    efi_exit_boot_services();
#endif

    // EFI memory map
#if defined (UEFI)
FEAT_START
    struct limine_efi_memmap_request *efi_memmap_request = get_request(LIMINE_EFI_MEMMAP_REQUEST);
    if (efi_memmap_request == NULL) {
        break; // next feature
    }

    struct limine_efi_memmap_response *efi_memmap_response =
        ext_mem_alloc(sizeof(struct limine_efi_memmap_response));

    efi_memmap_response->memmap = reported_addr(efi_mmap);
    efi_memmap_response->memmap_size = efi_mmap_size;
    efi_memmap_response->desc_size = efi_desc_size;
    efi_memmap_response->desc_version = efi_desc_ver;

    efi_memmap_request->response = reported_addr(efi_memmap_response);
FEAT_END
#endif

    if (base_revision < 3) {
        pmm_sanitiser_keep_first_page = false;
        pmm_sanitise_entries(memmap, &memmap_entries, true);
    }

    pagemap_t pagemap = {0};
    pagemap = build_pagemap(base_revision, nx_available, ranges, ranges_count,
                            physical_base, virtual_base, direct_map_offset);

    // MP
FEAT_START
    struct limine_mp_request *mp_request = get_request(LIMINE_MP_REQUEST);
    if (mp_request == NULL) {
        break; // next feature
    }

    struct limine_mp_info *mp_info;
    size_t cpu_count;
#if defined (__x86_64__) || defined (__i386__)
    uint32_t bsp_lapic_id;
    mp_info = init_smp(&cpu_count, &bsp_lapic_id,
                        paging_mode,
                        pagemap, mp_request->flags & LIMINE_MP_X2APIC, nx_available,
                        direct_map_offset, true);
#elif defined (__aarch64__)
    uint64_t bsp_mpidr;

    mp_info = init_smp(config, &cpu_count, &bsp_mpidr,
                        pagemap, LIMINE_MAIR(fb_attr), LIMINE_TCR(tsz, pa), LIMINE_SCTLR,
                        direct_map_offset);
#elif defined (__riscv)
    mp_info = init_smp(&cpu_count, pagemap, direct_map_offset);
#elif defined (__loongarch64)
    cpu_count = 0;
    mp_info = NULL; // TODO: LoongArch MP
#else
#error Unknown architecture
#endif

    if (mp_info == NULL) {
        break;
    }

    for (size_t i = 0; i < cpu_count; i++) {
#if defined (__x86_64__) || defined (__i386__)
        if (mp_info[i].lapic_id == bsp_lapic_id) {
            continue;
        }
#elif defined (__aarch64__)
        if (mp_info[i].mpidr == bsp_mpidr) {
            continue;
        }
#elif defined (__riscv)
        if (mp_info[i].hartid == bsp_hartid) {
            continue;
        }
#elif defined (__loongarch64)
#else
#error Unknown architecture
#endif

        void *cpu_stack = ext_mem_alloc(stack_size) + stack_size;
        mp_info[i].reserved = reported_addr(cpu_stack);
    }

    struct limine_mp_response *mp_response =
        ext_mem_alloc(sizeof(struct limine_mp_response));

#if defined (__x86_64__) || defined (__i386__)
    mp_response->flags |= (mp_request->flags & LIMINE_MP_X2APIC) && x2apic_check();
    mp_response->bsp_lapic_id = bsp_lapic_id;
#elif defined (__aarch64__)
    mp_response->bsp_mpidr = bsp_mpidr;
#elif defined (__riscv)
    mp_response->bsp_hartid = bsp_hartid;
#elif defined (__loongarch64)
#else
#error Unknown architecture
#endif

    uint64_t *mp_list = ext_mem_alloc(cpu_count * sizeof(uint64_t));
    for (size_t i = 0; i < cpu_count; i++) {
        mp_list[i] = reported_addr(&mp_info[i]);
    }

    mp_response->cpu_count = cpu_count;
    mp_response->cpus = reported_addr(mp_list);

    mp_request->response = reported_addr(mp_response);
FEAT_END

#if defined(__riscv)
    // RISC-V BSP Hart ID
FEAT_START
    struct limine_riscv_bsp_hartid_request *bsp_request = get_request(LIMINE_RISCV_BSP_HARTID_REQUEST);
    if (bsp_request == NULL) {
        break;
    }
    struct limine_riscv_bsp_hartid_response *bsp_response = ext_mem_alloc(sizeof(struct limine_riscv_bsp_hartid_response));
    bsp_response->bsp_hartid = bsp_hartid;
    bsp_request->response = reported_addr(bsp_response);
FEAT_END
#endif

    // Memmap
FEAT_START
    struct limine_memmap_request *memmap_request = get_request(LIMINE_MEMMAP_REQUEST);
    struct limine_memmap_response *memmap_response;
    struct limine_memmap_entry *_memmap;
    uint64_t *memmap_list;

    if (memmap_request != NULL) {
        memmap_response = ext_mem_alloc(sizeof(struct limine_memmap_response));
        _memmap = ext_mem_alloc(sizeof(struct limine_memmap_entry) * MEMMAP_MAX);
        memmap_list = ext_mem_alloc(MEMMAP_MAX * sizeof(uint64_t));
    }

    size_t mmap_entries;
    struct memmap_entry *mmap = get_memmap(&mmap_entries);

    if (memmap_request == NULL) {
        break; // next feature
    }

    if (mmap_entries > MEMMAP_MAX) {
        panic(false, "limine: Too many memmap entries");
    }

    for (size_t i = 0; i < mmap_entries; i++) {
        _memmap[i].base = mmap[i].base;
        _memmap[i].length = mmap[i].length;

        switch (mmap[i].type) {
            case MEMMAP_USABLE:
                _memmap[i].type = LIMINE_MEMMAP_USABLE;
                break;
            case MEMMAP_ACPI_RECLAIMABLE:
                _memmap[i].type = LIMINE_MEMMAP_ACPI_RECLAIMABLE;
                break;
            case MEMMAP_ACPI_NVS:
                _memmap[i].type = LIMINE_MEMMAP_ACPI_NVS;
                break;
            case MEMMAP_BAD_MEMORY:
                _memmap[i].type = LIMINE_MEMMAP_BAD_MEMORY;
                break;
            case MEMMAP_BOOTLOADER_RECLAIMABLE:
                _memmap[i].type = LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE;
                break;
            case MEMMAP_KERNEL_AND_MODULES:
                _memmap[i].type = LIMINE_MEMMAP_EXECUTABLE_AND_MODULES;
                break;
            case MEMMAP_FRAMEBUFFER:
                _memmap[i].type = LIMINE_MEMMAP_FRAMEBUFFER;
                break;
            default:
            case MEMMAP_RESERVED:
                _memmap[i].type = LIMINE_MEMMAP_RESERVED;
                break;
        }
    }

    for (size_t i = 0; i < mmap_entries; i++) {
        memmap_list[i] = reported_addr(&_memmap[i]);
    }

    memmap_response->entry_count = mmap_entries;
    memmap_response->entries = reported_addr(memmap_list);

    memmap_request->response = reported_addr(memmap_response);
FEAT_END

#if defined (__x86_64__) || defined (__i386__)
#if defined (BIOS)
    // If we're going 64, we might as well call this BIOS interrupt
    // to tell the BIOS that we are entering Long Mode, since it is in
    // the specification.
    struct rm_regs r = {0};
    r.eax = 0xec00;
    r.ebx = 0x02;   // Long mode only
    rm_int(0x15, &r, &r);
#endif

    pic_mask_all();
    io_apic_mask_all();

    irq_flush_type = IRQ_PIC_APIC_FLUSH;

    uint64_t reported_stack = reported_addr(stack);

    common_spinup(limine_spinup_32, 11,
        paging_mode, (uint32_t)(uintptr_t)pagemap.top_level,
        (uint32_t)entry_point, (uint32_t)(entry_point >> 32),
        (uint32_t)reported_stack, (uint32_t)(reported_stack >> 32),
        (uint32_t)(uintptr_t)local_gdt, nx_available,
        (uint32_t)direct_map_offset, (uint32_t)(direct_map_offset >> 32),
        (uint32_t)base_revision
    );
#elif defined (__aarch64__)
    vmm_assert_4k_pages();

    uint64_t reported_stack = reported_addr(stack);

    enter_in_el1(entry_point, reported_stack, LIMINE_SCTLR, LIMINE_MAIR(fb_attr), LIMINE_TCR(tsz, pa),
                 (uint64_t)pagemap.top_level[0],
                 (uint64_t)pagemap.top_level[1],
                 direct_map_offset);
#elif defined (__riscv)
    uint64_t reported_stack = reported_addr(stack);
    uint64_t satp = make_satp(pagemap.paging_mode, pagemap.top_level);

    riscv_spinup(entry_point, reported_stack, satp, direct_map_offset);
#elif defined (__loongarch64)
    uint64_t reported_stack = reported_addr(stack);

    loongarch_spinup(entry_point, reported_stack, (uint64_t)pagemap.pgd[0], (uint64_t)pagemap.pgd[1],
                     direct_map_offset);
#else
#error Unknown architecture
#endif
}
