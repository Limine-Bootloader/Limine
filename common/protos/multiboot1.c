#if defined (__x86_64__) || defined (__i386__)

#include <stdint.h>
#include <stddef.h>
#include <stdnoreturn.h>
#include <protos/multiboot1.h>
#include <protos/multiboot.h>
#include <config.h>
#include <lib/libc.h>
#include <lib/elf.h>
#include <lib/misc.h>
#include <lib/config.h>
#include <lib/print.h>
#include <lib/uri.h>
#include <lib/fb.h>
#include <lib/term.h>
#include <lib/elsewhere.h>
#include <sys/pic.h>
#include <sys/cpu.h>
#include <sys/idt.h>
#include <sys/iommu.h>
#include <sys/lapic.h>
#include <fs/file.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <drivers/vga_textmode.h>

#define LIMINE_BRAND "Limine " LIMINE_VERSION

#define MEMMAP_MAX 256

// Returns the size required to store the multiboot info.
static size_t get_multiboot1_info_size(
    char *cmdline,
    size_t modules_count, size_t modules_cmdlines_size,
    uint32_t section_entry_size, uint32_t section_num
) {
#define OVERFLOW panic(true, "multiboot1: info size overflow")
    return ALIGN_UP(sizeof(struct multiboot1_info), 16, OVERFLOW) +
           ALIGN_UP(strlen(cmdline) + 1, 16, OVERFLOW) +
           ALIGN_UP(sizeof(LIMINE_BRAND), 16, OVERFLOW) +
           ALIGN_UP(CHECKED_MUL(section_entry_size, section_num, OVERFLOW), 16, OVERFLOW) +
           ALIGN_UP(CHECKED_MUL(sizeof(struct multiboot1_module), modules_count, OVERFLOW), 16, OVERFLOW) +
           ALIGN_UP(modules_cmdlines_size, 16, OVERFLOW) +
           ALIGN_UP(sizeof(struct multiboot1_mmap_entry) * MEMMAP_MAX, 16, OVERFLOW);
#undef OVERFLOW
}

static void *mb1_info_alloc(void **mb1_info_raw, size_t size) {
    void *ret = *mb1_info_raw;
    *mb1_info_raw += ALIGN_UP(size, 16, panic(true, "multiboot: info alloc overflow"));
    return ret;
}

noreturn void multiboot1_load(char *config, char *cmdline) {
    struct file_handle *kernel_file;

    char *kernel_path = config_get_value(config, 0, "PATH");
    if (kernel_path == NULL) {
        kernel_path = config_get_value(config, 0, "KERNEL_PATH");
    }
    if (kernel_path == NULL) {
        panic(true, "multiboot1: Executable path not specified");
    }

    print("multiboot1: Loading executable `%#`...\n", kernel_path);

    if ((kernel_file = uri_open(kernel_path)) == NULL)
        panic(true, "multiboot1: Failed to open executable with path `%#`. Is the path correct?", kernel_path);

    uint8_t *kernel = freadall(kernel_file, MEMMAP_KERNEL_AND_MODULES);

    size_t kernel_file_size = kernel_file->size;

    fclose(kernel_file);

    struct multiboot1_header header = {0};
    size_t header_offset = 0;

    // Per Multiboot spec, header must be within first 8192 bytes and 4-byte aligned.
    // Ensure we don't read past end of file when checking magic or copying header.
    size_t search_limit = 8192;
    if (kernel_file_size < sizeof(struct multiboot1_header)) {
        panic(true, "multiboot1: Kernel file too small to contain header");
    }
    if (search_limit > kernel_file_size - sizeof(struct multiboot1_header)) {
        search_limit = kernel_file_size - sizeof(struct multiboot1_header);
    }

    for (header_offset = 0; header_offset <= search_limit; header_offset += 4) {
        uint32_t v = *(uint32_t *)(kernel + header_offset);

        if (v == MULTIBOOT1_HEADER_MAGIC) {
            memcpy(&header, kernel + header_offset, sizeof(header));
            break;
        }
    }

    if (header.magic != MULTIBOOT1_HEADER_MAGIC) {
        panic(true, "multiboot1: Invalid magic");
    }

    if (header.magic + header.flags + header.checksum)
        panic(true, "multiboot1: Header checksum is invalid");

    bool section_hdr_info_valid = false;
    struct elf_section_hdr_info section_hdr_info = {0};

    uint64_t entry_point;
    struct elsewhere_range *ranges;
    uint64_t ranges_count = 1;

    if (header.flags & (1 << 16)) {
        if (header.load_addr > header.header_addr)
            panic(true, "multiboot1: Illegal load address");

        size_t addr_diff = header.header_addr - header.load_addr;
        if (addr_diff > header_offset)
            panic(true, "multiboot1: Address tag offset underflow");

        size_t load_src = header_offset - addr_diff;

        size_t load_size;
        if (header.load_end_addr) {
            if (header.load_end_addr < header.load_addr)
                panic(true, "multiboot1: Load end address less than load address");
            load_size = header.load_end_addr - header.load_addr;
        } else {
            if (load_src > kernel_file_size)
                panic(true, "multiboot1: Load source exceeds kernel file size");
            load_size = kernel_file_size - load_src;
        }

        uint32_t bss_size = 0;
        if (header.bss_end_addr) {
            uintptr_t bss_addr = header.load_addr + load_size;
            if (header.bss_end_addr < bss_addr)
                panic(true, "multiboot1: Illegal bss end address");

            bss_size = header.bss_end_addr - bss_addr;
        }

        if (load_src + load_size > kernel_file_size) {
            panic(true, "multiboot1: load_src + load_size exceeds kernel file size");
        }

        size_t full_size = CHECKED_ADD(load_size, bss_size,
            panic(true, "multiboot1: load_size + bss_size overflow"));

        void *elsewhere = ext_mem_alloc(full_size);

        memcpy(elsewhere, kernel + load_src, load_size);

        entry_point = header.entry_addr;

        ranges = ext_mem_alloc(sizeof(struct elsewhere_range));

        ranges->elsewhere = (uintptr_t)elsewhere;
        ranges->target = header.load_addr;
        ranges->length = full_size;
    } else {
        int bits = elf_bits(kernel, kernel_file_size);

        switch (bits) {
            case 32:
                if (!elf32_load_elsewhere(kernel, kernel_file_size, &entry_point, &ranges))
                    panic(true, "multiboot1: ELF32 load failure");

                section_hdr_info = elf32_section_hdr_info(kernel, kernel_file_size);
                section_hdr_info_valid = true;
                break;
            case 64: {
                if (!elf64_load_elsewhere(kernel, kernel_file_size, &entry_point, &ranges))
                    panic(true, "multiboot1: ELF64 load failure");

                section_hdr_info = elf64_section_hdr_info(kernel, kernel_file_size);
                section_hdr_info_valid = true;
                break;
            }
            default:
                panic(true, "multiboot1: Invalid ELF file bitness");
        }
    }

    size_t n_modules;
    size_t modules_cmdlines_size = 0;

    for (n_modules = 0;; n_modules++) {
        struct conf_tuple conf_tuple = config_get_tuple(config, n_modules, "MODULE_PATH", "MODULE_STRING");
        if (!conf_tuple.value1) break;

        char *module_cmdline = conf_tuple.value2;
        if (!module_cmdline) module_cmdline = "";
        modules_cmdlines_size += ALIGN_UP(strlen(module_cmdline) + 1, 16, panic(true, "multiboot: info size overflow"));
    }

    size_t mb1_info_size = get_multiboot1_info_size(
        cmdline,
        n_modules,
        modules_cmdlines_size,
        section_hdr_info_valid ? section_hdr_info.section_entry_size : 0,
        section_hdr_info_valid ? section_hdr_info.num : 0
    );

    // Realloc elsewhere ranges to include mb1 info, modules, and elf sections
    uint64_t ranges_max = ranges_count
       + 1 /* mb1 info range */
       + n_modules
       + (section_hdr_info_valid ? section_hdr_info.num : 0);
    struct elsewhere_range *new_ranges = ext_mem_alloc_counted(ranges_max, sizeof(struct elsewhere_range));

    memcpy(new_ranges, ranges, sizeof(struct elsewhere_range) * ranges_count);
    pmm_free(ranges, sizeof(struct elsewhere_range) * ranges_count);
    ranges = new_ranges;

    // GRUB allocates boot info at 0x10000, *except* if the kernel happens
    // to overlap this region, then it gets moved to right after the
    // kernel, or whichever PHDR happens to sit at 0x10000.
    // Allocate it wherever, then move it to where GRUB puts it
    // afterwards.

    // Elsewhere append mb1 info *after* kernel but *before* modules.
    void *mb1_info_raw = ext_mem_alloc(mb1_info_size);
    uint64_t mb1_info_final_loc = 0x10000;

    if (!elsewhere_append(true /* flexible target */,
            ranges, &ranges_count, ranges_max,
            mb1_info_raw, &mb1_info_final_loc, mb1_info_size)) {
        panic(true, "multiboot1: Cannot allocate mb1 info");
    }

    size_t mb1_info_slide = (size_t)mb1_info_raw - mb1_info_final_loc;

    struct multiboot1_info *multiboot1_info =
        mb1_info_alloc(&mb1_info_raw, sizeof(struct multiboot1_info));

    if (section_hdr_info_valid == true) {
        size_t section_table_size = CHECKED_MUL(section_hdr_info.section_entry_size, section_hdr_info.num,
            panic(true, "multiboot1: ELF section table size overflow"));
        if (section_hdr_info.section_offset > kernel_file_size ||
            section_table_size > kernel_file_size - section_hdr_info.section_offset) {
            panic(true, "multiboot1: ELF section headers out of bounds");
        }

        multiboot1_info->elf_sect.num = section_hdr_info.num;
        multiboot1_info->elf_sect.size = section_hdr_info.section_entry_size;
        multiboot1_info->elf_sect.shndx = section_hdr_info.str_section_idx;

        void *sections = mb1_info_alloc(&mb1_info_raw, section_table_size);

        multiboot1_info->elf_sect.addr = (uintptr_t)sections - mb1_info_slide;

        memcpy(sections, kernel + section_hdr_info.section_offset, section_table_size);

        int bits = elf_bits(kernel, kernel_file_size);

        for (size_t i = 0; i < section_hdr_info.num; i++) {
            if (bits == 64) {
                struct elf64_shdr *shdr = (void *)sections + i * section_hdr_info.section_entry_size;

                if (shdr->sh_addr != 0 || shdr->sh_size == 0) {
                    continue;
                }

                if (shdr->sh_offset > kernel_file_size ||
                    shdr->sh_size > kernel_file_size - shdr->sh_offset) {
                    continue;
                }

                uint64_t section = (uint64_t)-1; /* no target preference, use top */

                if (!elsewhere_append(true /* flexible target */,
                        ranges, &ranges_count, ranges_max,
                        kernel + shdr->sh_offset, &section, shdr->sh_size)) {
                    panic(true, "multiboot1: Cannot allocate elf sections");
                }

                shdr->sh_addr = section;
            } else {
                struct elf32_shdr *shdr = (void *)sections + i * section_hdr_info.section_entry_size;

                if (shdr->sh_addr != 0 || shdr->sh_size == 0) {
                    continue;
                }

                if (shdr->sh_offset > kernel_file_size ||
                    shdr->sh_size > kernel_file_size - shdr->sh_offset) {
                    continue;
                }

                uint64_t section = (uint64_t)-1; /* no target preference, use top */

                if (!elsewhere_append(true /* flexible target */,
                        ranges, &ranges_count, ranges_max,
                        kernel + shdr->sh_offset, &section, shdr->sh_size)) {
                    panic(true, "multiboot1: Cannot allocate elf sections");
                }

                shdr->sh_addr = section;
            }
        }

        multiboot1_info->flags |= (1 << 5);
    }

    if (n_modules) {
        struct multiboot1_module *mods =
            mb1_info_alloc(&mb1_info_raw, sizeof(struct multiboot1_module) * n_modules);

        multiboot1_info->mods_count = n_modules;
        multiboot1_info->mods_addr = (size_t)mods - mb1_info_slide;

        for (size_t i = 0; i < n_modules; i++) {
            struct multiboot1_module *m = mods + i;

            struct conf_tuple conf_tuple = config_get_tuple(config, i, "MODULE_PATH", "MODULE_STRING");
            char *module_path = conf_tuple.value1;
            if (module_path == NULL)
                panic(true, "multiboot1: Module disappeared unexpectedly");

            print("multiboot1: Loading module `%#`...\n", module_path);

            struct file_handle *f;
            if ((f = uri_open(module_path)) == NULL)
                panic(true, "multiboot1: Failed to open module with path `%#`. Is the path correct?", module_path);

            char *module_cmdline = conf_tuple.value2;
            if (module_cmdline == NULL) {
                module_cmdline = "";
            }
            char *lowmem_modstr = mb1_info_alloc(&mb1_info_raw, strlen(module_cmdline) + 1);
            strcpy(lowmem_modstr, module_cmdline);

            void *module_addr = freadall(f, MEMMAP_BOOTLOADER_RECLAIMABLE);
            uint64_t module_target = (uint64_t)-1; /* no target preference, use top */

            if (!elsewhere_append(true /* flexible target */,
                    ranges, &ranges_count, ranges_max,
                    module_addr, &module_target, f->size)) {
                panic(true, "multiboot1: Cannot allocate module");
            }

            m->begin   = module_target;
            m->end     = m->begin + f->size;
            m->cmdline = (uint32_t)(size_t)lowmem_modstr - mb1_info_slide;
            m->pad     = 0;

            fclose(f);

            if (verbose) {
                print("multiboot1: Requested module %u:\n", (uint32_t)i);
                print("            Path:   %s\n", module_path);
                print("            String: \"%s\"\n", module_cmdline ?: "");
                print("            Begin:  %x\n", m->begin);
                print("            End:    %x\n", m->end);
            }
        }

        multiboot1_info->flags |= (1 << 3);
    }

    char *lowmem_cmdline = mb1_info_alloc(&mb1_info_raw, strlen(cmdline) + 1);
    strcpy(lowmem_cmdline, cmdline);
    multiboot1_info->cmdline = (uint32_t)(size_t)lowmem_cmdline - mb1_info_slide;
    multiboot1_info->flags |= (1 << 2);

    char *bootload_name = LIMINE_BRAND;
    char *lowmem_bootname = mb1_info_alloc(&mb1_info_raw, strlen(bootload_name) + 1);
    strcpy(lowmem_bootname, bootload_name);

    multiboot1_info->bootloader_name = (uint32_t)(size_t)lowmem_bootname - mb1_info_slide;
    multiboot1_info->flags |= (1 << 9);

    term_notready();

    size_t req_width = 0;
    size_t req_height = 0;
    size_t req_bpp = 0;
#if defined (BIOS)
    {
        char *textmode_str = config_get_value(config, 0, "TEXTMODE");
        bool textmode = textmode_str != NULL && strcmp(textmode_str, "yes") == 0;
        if (textmode) {
            goto textmode;
        }
    }
#endif


    if (header.flags & (1 << 2)) {
        req_width = header.fb_width;
        req_height = header.fb_height;
        req_bpp = header.fb_bpp;

        if (header.fb_mode == 0) {
#if defined (UEFI)
modeset:;
#endif
            char *resolution = config_get_value(config, 0, "RESOLUTION");
            if (resolution != NULL)
                parse_resolution(&req_width, &req_height, &req_bpp, resolution);

            struct fb_info *fbs;
            size_t fbs_count;
            fb_init(&fbs, &fbs_count, req_width, req_height, req_bpp, false);
            if (fbs_count == 0) {
#if defined (UEFI)
                goto skip_modeset;
#elif defined (BIOS)
textmode:
                vga_textmode_init(false);

                multiboot1_info->fb_addr    = 0xb8000;
                multiboot1_info->fb_width   = 80;
                multiboot1_info->fb_height  = 25;
                multiboot1_info->fb_bpp     = 16;
                multiboot1_info->fb_pitch   = 2 * 80;
                multiboot1_info->fb_type    = 2;
#endif
            } else {
                multiboot1_info->fb_addr    = (uint64_t)fbs[0].framebuffer_addr;
                multiboot1_info->fb_width   = fbs[0].framebuffer_width;
                multiboot1_info->fb_height  = fbs[0].framebuffer_height;
                multiboot1_info->fb_bpp     = fbs[0].framebuffer_bpp;
                multiboot1_info->fb_pitch   = fbs[0].framebuffer_pitch;
                multiboot1_info->fb_type    = 1;
                multiboot1_info->fb_red_mask_size    = fbs[0].red_mask_size;
                multiboot1_info->fb_red_mask_shift   = fbs[0].red_mask_shift;
                multiboot1_info->fb_green_mask_size  = fbs[0].green_mask_size;
                multiboot1_info->fb_green_mask_shift = fbs[0].green_mask_shift;
                multiboot1_info->fb_blue_mask_size   = fbs[0].blue_mask_size;
                multiboot1_info->fb_blue_mask_shift  = fbs[0].blue_mask_shift;
            }
        } else {
#if defined (UEFI)
            print("multiboot1: Warning: Cannot use text mode with UEFI\n");
            goto modeset;
#elif defined (BIOS)
            goto textmode;
#endif
        }

        multiboot1_info->flags |= (1 << 12);

#if defined (UEFI)
skip_modeset:;
#endif
    } else {
#if defined (UEFI)
        panic(true, "multiboot1: Cannot use text mode with UEFI.");
#elif defined (BIOS)
        vga_textmode_init(false);
#endif
    }

    // Load relocation stub where it won't get overwritten (hopefully)
    size_t reloc_stub_size = (size_t)multiboot_reloc_stub_end - (size_t)multiboot_reloc_stub;
    void *reloc_stub = ext_mem_alloc(reloc_stub_size);
    memcpy(reloc_stub, multiboot_reloc_stub, reloc_stub_size);

#if defined (UEFI)
    efi_exit_boot_services();
#endif

    size_t mb_mmap_count;
    struct memmap_entry *raw_memmap = get_raw_memmap(&mb_mmap_count);

    if (mb_mmap_count > MEMMAP_MAX) {
        panic(false, "multiboot1: Too many memory map entries.");
    }

    size_t mb_mmap_len = mb_mmap_count * sizeof(struct multiboot1_mmap_entry);
    struct multiboot1_mmap_entry *mmap = mb1_info_alloc(&mb1_info_raw, mb_mmap_len);

    // Multiboot is bad and passes raw memmap. We do the same to support it.
    for (size_t i = 0; i < mb_mmap_count; i++) {
        mmap[i].size = sizeof(struct multiboot1_mmap_entry) - 4;
        mmap[i].addr = raw_memmap[i].base;
        mmap[i].len  = raw_memmap[i].length;
        mmap[i].type = raw_memmap[i].type;
    }

    struct meminfo memory_info = mmap_get_info(mb_mmap_count, raw_memmap);

    // Convert the uppermem and lowermem fields from bytes to
    // KiB.
    multiboot1_info->mem_lower = memory_info.lowermem / 1024;
    multiboot1_info->mem_upper = memory_info.uppermem / 1024;

    multiboot1_info->mmap_length = mb_mmap_len;
    multiboot1_info->mmap_addr = (uint32_t)(size_t)mmap - mb1_info_slide;
    multiboot1_info->flags |= (1 << 0) | (1 << 6);

    if (rdmsr(0x1b) & (1 << 10)) {
        if (x2apic_disable()) {
            printv("multiboot1: Firmware had x2APIC enabled, reverted to xAPIC mode\n");
        } else {
            printv("multiboot1: Firmware has x2APIC enabled and it could not be disabled\n");
        }
    }

    iommu_disable_all();

    irq_flush_type = IRQ_PIC_ONLY_FLUSH;

    common_spinup(multiboot_spinup_32, 6,
                  (uint32_t)(uintptr_t)reloc_stub, (uint32_t)0x2badb002,
                  (uint32_t)mb1_info_final_loc, (uint32_t)entry_point,
                  (uint32_t)(uintptr_t)ranges, (uint32_t)ranges_count);
}

#endif
