#if defined (__x86_64__) || defined (__i386__)

#include <stdint.h>
#include <stddef.h>
#include <stdnoreturn.h>
#include <protos/linux.h>
#include <fs/file.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/real.h>
#include <lib/term.h>
#include <lib/config.h>
#include <lib/print.h>
#include <lib/uri.h>
#include <lib/tpm.h>
#include <mm/pmm.h>
#include <sys/idt.h>
#include <lib/fb.h>
#include <lib/acpi.h>
#include <sys/iommu.h>
#include <sys/cpu.h>
#include <sys/lapic.h>
#include <drivers/edid.h>
#include <drivers/vga_textmode.h>
#include <drivers/gop.h>

noreturn void linux_spinup(void *entry, void *boot_params);
#if defined (UEFI) && defined (__x86_64__)
    noreturn void linux_spinup64(void *entry, void *boot_params);
#endif

// Fields Limine leaves alone are reserved bytes, named after where they sit in
// the zero page.

// Linux Documentation/arch/x86/boot.rst, "The Real-Mode Kernel Header".
struct setup_header {
    uint8_t reserved_1f1[1];
    uint8_t reserved_1f2[2];
    uint8_t reserved_1f4[4];
    uint8_t reserved_1f8[2];
    uint16_t vid_mode;
    uint8_t reserved_1fc[2];
    uint8_t reserved_1fe[2];
    uint8_t reserved_200[2];
    uint8_t reserved_202[4];
    uint16_t version;
    uint8_t reserved_208[4];
    uint8_t reserved_20c[2];
    uint16_t kernel_version;
    uint8_t type_of_loader;
    uint8_t loadflags;
    uint8_t reserved_212[2];
    uint8_t reserved_214[4];
    uint32_t ramdisk_image;
    uint32_t ramdisk_size;
    uint8_t reserved_220[4];
    uint8_t reserved_224[2];
    uint8_t reserved_226[1];
    uint8_t reserved_227[1];
    uint32_t cmd_line_ptr;
    uint32_t initrd_addr_max;
    uint32_t kernel_alignment;
    uint8_t relocatable_kernel;
    uint8_t reserved_235[1];
    uint16_t xloadflags;
    uint8_t reserved_238[4];
    uint8_t reserved_23c[4];
    uint8_t reserved_240[8];
    uint8_t reserved_248[4];
    uint8_t reserved_24c[4];
    uint64_t setup_data;
    uint64_t pref_address;
    uint32_t init_size;
    uint8_t reserved_264[4];
    uint8_t reserved_268[4];
} __attribute__((packed));

_Static_assert(sizeof(struct setup_header) == 0x26c - 0x1f1, "setup_header layout");

// The INT 15h E820h address range descriptor without its extended attributes
// (ACPI 6.6, section 15.1, table 15.4).
struct boot_e820_entry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
} __attribute__((packed));

_Static_assert(offsetof(struct boot_e820_entry, addr) == 0, "boot_e820_entry layout");
_Static_assert(offsetof(struct boot_e820_entry, size) == 8, "boot_e820_entry layout");
_Static_assert(offsetof(struct boot_e820_entry, type) == 16, "boot_e820_entry layout");
_Static_assert(sizeof(struct boot_e820_entry) == 20, "boot_e820_entry layout");

#define E820_MAX_ENTRIES_ZEROPAGE 128

struct efi_info {
    uint32_t efi_loader_signature;
    uint32_t efi_systab;
    uint32_t efi_memdesc_size;
    uint32_t efi_memdesc_version;
    uint32_t efi_memmap;
    uint32_t efi_memmap_size;
    uint32_t efi_systab_hi;
    uint32_t efi_memmap_hi;
};

_Static_assert(offsetof(struct efi_info, efi_loader_signature) == 0x00, "efi_info layout");
_Static_assert(offsetof(struct efi_info, efi_systab) == 0x04, "efi_info layout");
_Static_assert(offsetof(struct efi_info, efi_memdesc_size) == 0x08, "efi_info layout");
_Static_assert(offsetof(struct efi_info, efi_memdesc_version) == 0x0c, "efi_info layout");
_Static_assert(offsetof(struct efi_info, efi_memmap) == 0x10, "efi_info layout");
_Static_assert(offsetof(struct efi_info, efi_memmap_size) == 0x14, "efi_info layout");
_Static_assert(offsetof(struct efi_info, efi_systab_hi) == 0x18, "efi_info layout");
_Static_assert(offsetof(struct efi_info, efi_memmap_hi) == 0x1c, "efi_info layout");
_Static_assert(sizeof(struct efi_info) == 0x20, "efi_info layout");

// boot.rst, "setup_data".
struct setup_data {
    uint64_t next;
    uint32_t type;
    uint32_t len;
    uint8_t data[];
};

_Static_assert(offsetof(struct setup_data, next) == 0, "setup_data layout");
_Static_assert(offsetof(struct setup_data, type) == 8, "setup_data layout");
_Static_assert(offsetof(struct setup_data, len) == 12, "setup_data layout");
_Static_assert(offsetof(struct setup_data, data) == 16, "setup_data layout");
_Static_assert(sizeof(struct setup_data) == 16, "setup_data layout");

// The setup_data type that carries the memory map entries the zero page has no
// room for.
#define SETUP_E820_EXT 1

// Linux Documentation/arch/x86/zero-page.rst. That table leaves out the setup
// header, which boot.rst, "32-bit Boot Protocol", places at 0x1f1.
struct boot_params {
    struct screen_info screen_info;
    uint8_t reserved_040[0x14];
    uint8_t reserved_054[4];
    uint8_t reserved_058[8];
    uint8_t reserved_060[0x10];
    uint64_t acpi_rsdp_addr;
    uint8_t reserved_078[8];
    uint8_t reserved_080[0x10];
    uint8_t reserved_090[0x10];
    uint8_t reserved_0a0[0x10];
    uint8_t reserved_0b0[0x10];
    uint32_t ext_ramdisk_image;
    uint32_t ext_ramdisk_size;
    uint8_t reserved_0c8[4];
    uint8_t reserved_0cc[0x70];
    uint8_t reserved_13c[4];
    struct edid_info_struct edid_info;
    struct efi_info efi_info;
    uint8_t reserved_1e0[4];
    uint8_t reserved_1e4[4];
    uint8_t e820_entries;
    uint8_t reserved_1e9[1];
    uint8_t reserved_1ea[1];
    uint8_t reserved_1eb[1];
    uint8_t secure_boot;
    uint8_t reserved_1ed[2];
    uint8_t reserved_1ef[1];
    uint8_t reserved_1f0[1];
    struct setup_header hdr;
    uint8_t reserved_26c[0x24];
    uint8_t edd_mbr_sig_buffer[0x40];
    struct boot_e820_entry e820_table[E820_MAX_ENTRIES_ZEROPAGE];
    uint8_t reserved_cd0[0x30];
    uint8_t reserved_d00[0x1ec];
    uint8_t reserved_eec[0x114];
} __attribute__((packed));

_Static_assert(offsetof(struct boot_params, screen_info) == 0x000, "boot_params layout");
_Static_assert(offsetof(struct boot_params, acpi_rsdp_addr) == 0x070, "boot_params layout");
_Static_assert(offsetof(struct boot_params, ext_ramdisk_image) == 0x0c0, "boot_params layout");
_Static_assert(offsetof(struct boot_params, ext_ramdisk_size) == 0x0c4, "boot_params layout");
_Static_assert(offsetof(struct boot_params, edid_info) == 0x140, "boot_params layout");
_Static_assert(sizeof(((struct boot_params *)0)->edid_info) == 0x80, "boot_params layout");
_Static_assert(offsetof(struct boot_params, efi_info) == 0x1c0, "boot_params layout");
_Static_assert(offsetof(struct boot_params, e820_entries) == 0x1e8, "boot_params layout");
_Static_assert(offsetof(struct boot_params, secure_boot) == 0x1ec, "boot_params layout");
_Static_assert(offsetof(struct boot_params, hdr) == 0x1f1, "boot_params layout");
_Static_assert(offsetof(struct boot_params, edd_mbr_sig_buffer) == 0x290, "boot_params layout");
_Static_assert(offsetof(struct boot_params, e820_table) == 0x2d0, "boot_params layout");
_Static_assert(sizeof(((struct boot_params *)0)->e820_table) == 0xa00, "boot_params layout");
_Static_assert(sizeof(struct boot_params) == 0x1000, "boot_params layout");

_Static_assert(offsetof(struct boot_params, hdr.vid_mode) == 0x1fa, "setup_header layout");
_Static_assert(offsetof(struct boot_params, hdr.version) == 0x206, "setup_header layout");
_Static_assert(offsetof(struct boot_params, hdr.kernel_version) == 0x20e, "setup_header layout");
_Static_assert(offsetof(struct boot_params, hdr.type_of_loader) == 0x210, "setup_header layout");
_Static_assert(offsetof(struct boot_params, hdr.loadflags) == 0x211, "setup_header layout");
_Static_assert(offsetof(struct boot_params, hdr.ramdisk_image) == 0x218, "setup_header layout");
_Static_assert(offsetof(struct boot_params, hdr.ramdisk_size) == 0x21c, "setup_header layout");
_Static_assert(offsetof(struct boot_params, hdr.cmd_line_ptr) == 0x228, "setup_header layout");
_Static_assert(offsetof(struct boot_params, hdr.initrd_addr_max) == 0x22c, "setup_header layout");
_Static_assert(offsetof(struct boot_params, hdr.kernel_alignment) == 0x230, "setup_header layout");
_Static_assert(offsetof(struct boot_params, hdr.relocatable_kernel) == 0x234, "setup_header layout");
_Static_assert(offsetof(struct boot_params, hdr.xloadflags) == 0x236, "setup_header layout");
_Static_assert(offsetof(struct boot_params, hdr.setup_data) == 0x250, "setup_header layout");
_Static_assert(offsetof(struct boot_params, hdr.pref_address) == 0x258, "setup_header layout");
_Static_assert(offsetof(struct boot_params, hdr.init_size) == 0x260, "setup_header layout");

#define LINUX_VER(maj, min) (((uint32_t)(maj) << 16) | (uint32_t)(min))

// Returns the kernel's version as LINUX_VER(), or 0 when it cannot be read.
// header.S always emits the pointer, so a kernel without one is not Linux.
static uint32_t linux_version_of(struct file_handle *kernel_file,
                                 struct setup_header *setup_header) {
    if (setup_header->kernel_version == 0) {
        return 0;
    }

    size_t offset = (size_t)setup_header->kernel_version + 0x200;
    if (offset >= kernel_file->size) {
        return 0;
    }

    char buf[32];
    size_t avail = kernel_file->size - offset;
    size_t len = avail < sizeof(buf) - 1 ? avail : sizeof(buf) - 1;
    fread(kernel_file, buf, offset, len);
    buf[len] = '\0';

    uint32_t major = 0, minor = 0;
    size_t i = 0;
    if (buf[i] < '0' || buf[i] > '9') {
        return 0;
    }
    for (; i < len && buf[i] >= '0' && buf[i] <= '9'; i++) {
        major = major * 10 + (uint32_t)(buf[i] - '0');
    }
    if (i >= len || buf[i] != '.') {
        return 0;
    }
    i++;
    if (i >= len || buf[i] < '0' || buf[i] > '9') {
        return 0;
    }
    for (; i < len && buf[i] >= '0' && buf[i] <= '9'; i++) {
        minor = minor * 10 + (uint32_t)(buf[i] - '0');
    }

    return LINUX_VER(major, minor);
}

noreturn void linux_load(char *config, char *cmdline) {
    struct file_handle *kernel_file;

#if defined (UEFI)
    if (cmdline != NULL) {
        tpm_measure(TPM_PCR_BOOT_AUTH, TPM_EV_IPL,
                    cmdline, strlen(cmdline), "cmdline: ", cmdline);
    }
#endif

    char *kernel_path = config_get_value(config, 0, "PATH");
    if (kernel_path == NULL) {
        kernel_path = config_get_value(config, 0, "KERNEL_PATH");
    }
    if (kernel_path == NULL) {
        panic(true, "linux: Kernel path not specified");
    }

    if (!terse) {
        print("linux: Loading kernel `%#`...\n", kernel_path);
    }

    if ((kernel_file = uri_open(kernel_path, MEMMAP_BOOTLOADER_RECLAIMABLE,
#if defined (__i386__)
        false, NULL, NULL
#else
        true
#endif
    )) == NULL)
        panic(true, "linux: Failed to open kernel with path `%#`. Is the path correct?", kernel_path);

    // Minimum size check: need at least 0x206 bytes for signature at 0x202
    if (kernel_file->size < 0x206) {
        panic(true, "linux: Kernel file too small");
    }

#if defined (UEFI) && defined (__x86_64__)
    bool use_64_bit_proto = false;
#endif

    uint32_t signature;
    fread(kernel_file, &signature, 0x202, sizeof(uint32_t));

    // validate signature
    if (signature != 0x53726448) {
        panic(true, "linux: Invalid kernel signature");
    }

    size_t setup_code_size = 0;
    fread(kernel_file, &setup_code_size, 0x1f1, 1);

    if (setup_code_size == 0)
        setup_code_size = 4;

    setup_code_size *= 512;

    size_t real_mode_code_size = 512 + setup_code_size;

    if (real_mode_code_size > kernel_file->size) {
        panic(true, "linux: Kernel file too small for real mode code");
    }

    struct boot_params *boot_params = ext_mem_alloc(sizeof(struct boot_params));

    struct setup_header *setup_header = &boot_params->hdr;

    size_t setup_header_end = ({
        uint8_t x;
        fread(kernel_file, &x, 0x201, 1);
        0x202 + x;
    });

    if (setup_header_end > kernel_file->size) {
        panic(true, "linux: Kernel file too small for setup header");
    }

    // The zero page only reserves up to edd_mbr_sig_buffer for the setup
    // header, so a longer one cannot be copied in without overwriting fields
    // past it.
    if (setup_header_end > offsetof(struct boot_params, edd_mbr_sig_buffer)) {
        panic(true, "linux: Setup header too long for the zero page");
    }

    fread(kernel_file, setup_header, 0x1f1, setup_header_end - 0x1f1);

    printv("linux: Boot protocol: %u.%u\n",
           setup_header->version >> 8, setup_header->version & 0xff);

    if (setup_header->version < 0x202) {
        panic(true, "linux: Protocols < 2.02 are not supported");
    }

    setup_header->cmd_line_ptr = (uint32_t)(uintptr_t)cmdline;

    // vid_mode. 0xffff means "normal"
    setup_header->vid_mode = 0xffff;

    // Read while the file is still open; it is consulted after the handoff
    // quirks below, long after fclose().
    uint32_t linux_ver = linux_version_of(kernel_file, setup_header);

    if (verbose) {
        char *kernel_version = ext_mem_alloc(128);
        if (setup_header->kernel_version != 0) {
            size_t version_offset = (size_t)setup_header->kernel_version + 0x200;
            if (version_offset + 128 <= kernel_file->size) {
                fread(kernel_file, kernel_version, version_offset, 128);
                kernel_version[127] = '\0';
                print("linux: Kernel version: %s\n", kernel_version);
            }
        }
        pmm_free(kernel_version, 128);
    }

    setup_header->type_of_loader = 0xff;

    if (!(setup_header->loadflags & (1 << 0))) {
        panic(true, "linux: Kernels that load at 0x10000 are not supported");
    }

    setup_header->loadflags &= ~(1 << 5);     // print early messages

    // load kernel
    size_t kernel_data_size = kernel_file->size - real_mode_code_size;
    size_t kernel_alloc_size = kernel_data_size;
    if (setup_header->version >= 0x20a && setup_header->init_size > kernel_alloc_size) {
        kernel_alloc_size = setup_header->init_size;
    }
    uintptr_t kernel_align = 0x100000;
    if (setup_header->version >= 0x205 && setup_header->kernel_alignment > kernel_align) {
        kernel_align = setup_header->kernel_alignment;
        if ((kernel_align & (kernel_align - 1)) != 0) {
            panic(true, "linux: kernel_alignment is not a power of two");
        }
    }
    // Start at pref_address: the decompressor relocates itself up to
    // LOAD_PHYSICAL_ADDR (= pref_address) and scribbles init_size bytes from
    // there, so loading below it would leave that range unreserved.
    // XLF_KERNEL_64 with XLF_CAN_BE_LOADED_ABOVE_4G is the kernel saying it has a
    // 64-bit entry point and may sit above 4GiB; otherwise the handoff is 32-bit.
    uint64_t kernel_addr_limit = 0xffffffff;
#if defined (UEFI) && defined (__x86_64__)
    // xloadflags only exists from 2.12; below that the field is padding.
    bool xlf_64bit_entry = setup_header->version >= 0x20c
                        && (setup_header->xloadflags & 3) == 3;

    if (xlf_64bit_entry) {
        kernel_addr_limit = UINT64_MAX;
    }
#endif
    uintptr_t kernel_search_start = 0x100000;
    if (setup_header->version >= 0x20a
     && setup_header->pref_address >= 0x100000
     && kernel_alloc_size <= kernel_addr_limit
     && setup_header->pref_address <= kernel_addr_limit - kernel_alloc_size) {
        kernel_search_start = (uintptr_t)setup_header->pref_address;
    }
    // Non-relocatable kernels must be loaded at their required address; do
    // not step up on failure.
    bool relocatable_kernel = setup_header->version >= 0x205
                           && setup_header->relocatable_kernel != 0;
    // The walk gets the ceiling the search start already has, so that both ends
    // of the range agree about what the handoff can express.
    uint64_t kernel_addr_max = 0;
    if (kernel_alloc_size <= kernel_addr_limit) {
        kernel_addr_max = kernel_addr_limit - kernel_alloc_size;
    }
    uintptr_t kernel_load_addr = ALIGN_UP(kernel_search_start, kernel_align, panic(true, "linux: Alignment overflow"));
    // The loop bounds each step, not the address the walk starts from, and
    // aligning up can pass the ceiling the search start was checked against.
    if ((uint64_t)kernel_load_addr > kernel_addr_max) {
        panic(true, "linux: Failed to allocate memory for kernel");
    }
    for (;;) {
        if (memmap_alloc_range(kernel_load_addr,
                ALIGN_UP(kernel_alloc_size, 4096, panic(true, "linux: Alignment overflow")),
                MEMMAP_BOOTLOADER_RECLAIMABLE, MEMMAP_USABLE, false, false, false))
            break;

        if (!relocatable_kernel) {
            panic(true, "linux: Non-relocatable kernel could not be loaded at required address %X", (uint64_t)kernel_load_addr);
        }

        // The first bound is not a multiple of the alignment, so the step can
        // pass it rather than land on it; the second is what the handoff can
        // express, and passing that is what truncates the address to zero.
        if (kernel_load_addr >= 0xfff00000
         || (uint64_t)kernel_load_addr + kernel_align > kernel_addr_max) {
            panic(true, "linux: Failed to allocate memory for kernel");
        }

        kernel_load_addr = CHECKED_ADD(kernel_load_addr, kernel_align,
                panic(true, "linux: Failed to allocate memory for kernel"));
    }

#if defined (UEFI) && defined (__x86_64__)
    if (kernel_load_addr > 0xffffffff) {
        use_64_bit_proto = true;
    }
#endif

    fread(kernel_file, (void *)kernel_load_addr, real_mode_code_size, kernel_file->size - real_mode_code_size);

#if defined (UEFI)
    tpm_measure_path(TPM_PCR_BOOT_AUTH, TPM_EV_IPL, "path: ", kernel_path);
    tpm_measure(TPM_PCR_LOADED_IMAGES, TPM_EV_IPL,
                kernel_file->fd, kernel_file->size, "path: ", kernel_path);
#endif

    fclose(kernel_file);

    ///////////////////////////////////////
    // Modules
    ///////////////////////////////////////
    size_t size_of_all_modules = 0;

    size_t module_count;
    for (module_count = 0; ; module_count++) {
        char *module_path = config_get_value(config, module_count, "MODULE_PATH");
        if (module_path == NULL)
            break;
    }

    if (module_count == 0) {
        goto no_modules;
    }

    struct file_handle **modules = ext_mem_alloc_counted(module_count, sizeof(struct file_handle *));

    for (size_t i = 0; ; i++) {
        char *module_path = config_get_value(config, i, "MODULE_PATH");
        if (module_path == NULL)
            break;

        if (!terse) {
            print("linux: Loading module `%#`...\n", module_path);
        }

        struct file_handle *module;
        if ((module = uri_open(module_path, MEMMAP_BOOTLOADER_RECLAIMABLE,
#if defined (__i386__)
            false, NULL, NULL
#else
            true
#endif
        )) == NULL)
            panic(true, "linux: Failed to open module with path `%s`. Is the path correct?", module_path);

        // Align each module to 4 bytes so the kernel's initramfs unpacker,
        // which only accepts a raw cpio header at a 4-byte aligned offset,
        // can find concatenated archives.
        size_t module_size = ALIGN_UP(module->size, 4,
            panic(true, "linux: Total module size overflow"));
        size_of_all_modules = CHECKED_ADD(size_of_all_modules, module_size,
            panic(true, "linux: Total module size overflow"));

        modules[i] = module;
    }

    uint64_t modules_mem_base;

    if (setup_header->version <= 0x202 || setup_header->initrd_addr_max == 0) {
        modules_mem_base = 0x38000000;
    } else {
        modules_mem_base = (uint64_t)setup_header->initrd_addr_max + 1;
    }

    if (size_of_all_modules > modules_mem_base) {
        panic(true, "linux: Total module size exceeds available address space");
    }
    modules_mem_base -= size_of_all_modules;
    modules_mem_base = ALIGN_DOWN(modules_mem_base, 0x100000);

    for (;;) {
        if (modules_mem_base < 0x100000) {
#if defined (UEFI) && defined (__x86_64__)
            if (xlf_64bit_entry) {
                modules_mem_base = (uintptr_t)ext_mem_alloc_type_aligned_mode(
                    size_of_all_modules,
                    MEMMAP_BOOTLOADER_RECLAIMABLE,
                    0x200000,
                    true
                );
                use_64_bit_proto = true;
                break;
            }
#endif
            panic(true, "linux: Failed to allocate memory for modules");
        }

        if (memmap_alloc_range(modules_mem_base, ALIGN_UP(size_of_all_modules, 0x100000, panic(true, "linux: Alignment overflow")),
                               MEMMAP_BOOTLOADER_RECLAIMABLE, MEMMAP_USABLE, false, false, false))
            break;

        modules_mem_base -= 0x100000;
    }

    uintptr_t _modules_mem_base = modules_mem_base;
    for (size_t i = 0; ; i++) {
        char *module_path = config_get_value(config, i, "MODULE_PATH");
        if (module_path == NULL)
            break;

        size_t module_size = modules[i]->size;
        size_t padded_size = ALIGN_UP(module_size, 4,
            panic(true, "linux: Total module size overflow"));

        fread(modules[i], (void *)_modules_mem_base, 0, module_size);
        memset((void *)(_modules_mem_base + module_size), 0,
               padded_size - module_size);

#if defined (UEFI)
        tpm_measure_path(TPM_PCR_BOOT_AUTH, TPM_EV_IPL, "module_path: ", module_path);
        tpm_measure(TPM_PCR_LOADED_IMAGES, TPM_EV_IPL,
                    (void *)_modules_mem_base, module_size, "module_path: ", module_path);
#endif

        _modules_mem_base += padded_size;

        fclose(modules[i]);
    }

    pmm_free(modules, module_count * sizeof(struct file_handle *));

    setup_header->ramdisk_image = (uint32_t)modules_mem_base;
#if defined (UEFI) && defined (__x86_64__)
    boot_params->ext_ramdisk_image = (uint32_t)(modules_mem_base >> 32);
#endif
    setup_header->ramdisk_size = (uint32_t)size_of_all_modules;
#if defined (UEFI) && defined (__x86_64__)
    boot_params->ext_ramdisk_size = (uint32_t)(size_of_all_modules >> 32);
#endif

no_modules:;

    ///////////////////////////////////////
    // Video
    ///////////////////////////////////////

    term_notready();

    struct screen_info *screen_info = &boot_params->screen_info;

#if defined (BIOS)
    {
    char *textmode_str = config_get_value(config, 0, "TEXTMODE");
    bool textmode = textmode_str != NULL && strcmp(textmode_str, "yes") == 0;
    if (textmode) {
        goto set_textmode;
    }
    }
#endif

    size_t req_width = 0, req_height = 0, req_bpp = 0;

    char *resolution = config_get_value(config, 0, "RESOLUTION");
    if (resolution != NULL)
        parse_resolution(&req_width, &req_height, &req_bpp, resolution);

    struct fb_info *fbs;
    size_t fbs_count;
#if defined (UEFI)
    gop_force_16 = true;
#endif
    fb_init(&fbs, &fbs_count, req_width, req_height, req_bpp, false, false);
    if (fbs_count == 0) {
#if defined (UEFI)
        goto no_fb;
#elif defined (BIOS)
set_textmode:;
        vga_textmode_init(false);

        screen_info->orig_video_mode = 3;
        screen_info->orig_video_ega_bx = 3;
        screen_info->orig_video_lines = 25;
        screen_info->orig_video_cols = 80;
        screen_info->orig_video_points = 16;

        screen_info->orig_video_isVGA = VIDEO_TYPE_VGAC;
#endif
    } else {
        screen_info->capabilities   = VIDEO_CAPABILITY_64BIT_BASE;
        screen_info->flags          = VIDEO_FLAGS_NOCURSOR;
        screen_info->lfb_base       = (uint32_t)fbs[0].framebuffer_addr;
        screen_info->ext_lfb_base   = (uint32_t)(fbs[0].framebuffer_addr >> 32);
        screen_info->lfb_size       = fbs[0].framebuffer_pitch * fbs[0].framebuffer_height;
        screen_info->lfb_width      = fbs[0].framebuffer_width;
        screen_info->lfb_height     = fbs[0].framebuffer_height;
        screen_info->lfb_depth      = fbs[0].framebuffer_bpp;
        screen_info->lfb_linelength = fbs[0].framebuffer_pitch;
        screen_info->red_size       = fbs[0].red_mask_size;
        screen_info->red_pos        = fbs[0].red_mask_shift;
        screen_info->green_size     = fbs[0].green_mask_size;
        screen_info->green_pos      = fbs[0].green_mask_shift;
        screen_info->blue_size      = fbs[0].blue_mask_size;
        screen_info->blue_pos       = fbs[0].blue_mask_shift;

        if (fbs[0].edid != NULL) {
            memcpy(&boot_params->edid_info, fbs[0].edid, sizeof(struct edid_info_struct));
        }

#if defined (BIOS)
        screen_info->orig_video_isVGA = VIDEO_TYPE_VLFB;
        screen_info->lfb_size = DIV_ROUNDUP(screen_info->lfb_size, 65536, panic(true, "linux: Alignment overflow"));
#elif defined (UEFI)
        screen_info->orig_video_isVGA = VIDEO_TYPE_EFI;
#endif
    }

#if defined (UEFI)
no_fb:;
#endif
    ///////////////////////////////////////
    // RSDP
    ///////////////////////////////////////

    boot_params->acpi_rsdp_addr = (uintptr_t)acpi_get_rsdp();

    ///////////////////////////////////////
    // e820 overflow table
    ///////////////////////////////////////

    // Finalising the memory map closes the allocator, and on UEFI that cannot
    // happen until after ExitBootServices, so reserve the table up front.
    struct setup_data *e820_ext = NULL;
    size_t e820_ext_max = 0;

    if (setup_header->version >= 0x209) {
        size_t max_entries = get_raw_memmap_max_entries();
        if (max_entries > E820_MAX_ENTRIES_ZEROPAGE) {
            e820_ext_max = max_entries - E820_MAX_ENTRIES_ZEROPAGE;
            e820_ext = ext_mem_alloc(sizeof(struct setup_data)
                                     + e820_ext_max * sizeof(struct boot_e820_entry));
        }
    }

    ///////////////////////////////////////
    // UEFI
    ///////////////////////////////////////
#if defined (UEFI)
    linux_install_efi_tpm_event_log();
    efi_exit_boot_services();

#if defined (__x86_64__)
    memcpy(&boot_params->efi_info.efi_loader_signature, "EL64", 4);
#elif defined (__i386__)
    memcpy(&boot_params->efi_info.efi_loader_signature, "EL32", 4);
#endif

    boot_params->efi_info.efi_systab    = (uint32_t)(uint64_t)(uintptr_t)gST;
    boot_params->efi_info.efi_systab_hi = (uint32_t)((uint64_t)(uintptr_t)gST >> 32);
    boot_params->efi_info.efi_memmap    = (uint32_t)(uint64_t)(uintptr_t)efi_mmap;
    boot_params->efi_info.efi_memmap_hi = (uint32_t)((uint64_t)(uintptr_t)efi_mmap >> 32);
    boot_params->efi_info.efi_memmap_size     = efi_mmap_size;
    boot_params->efi_info.efi_memdesc_size    = efi_desc_size;
    boot_params->efi_info.efi_memdesc_version = efi_desc_ver;

    boot_params->secure_boot = secure_boot_active ? 3 : 2;
#endif

    ///////////////////////////////////////
    // e820
    ///////////////////////////////////////

    struct boot_e820_entry *e820_table = boot_params->e820_table;

    size_t mmap_entries;
    struct memmap_entry *mmap = get_raw_memmap(&mmap_entries);

    struct boot_e820_entry *e820_ext_table = e820_ext == NULL
        ? NULL : (struct boot_e820_entry *)e820_ext->data;
    size_t j = 0, k = 0;

    for (size_t i = 0; i < mmap_entries; i++) {
        if (mmap[i].type >= 0x1000) {
            continue;
        }

        struct boot_e820_entry *entry;
        if (j < E820_MAX_ENTRIES_ZEROPAGE) {
            entry = &e820_table[j++];
            boot_params->e820_entries = j;
        } else if (k < e820_ext_max) {
            entry = &e820_ext_table[k++];
        } else {
            panic(false, "linux: Too many E820 memory map entries");
        }

        entry->addr = mmap[i].base;
        entry->size = mmap[i].length;
        entry->type = mmap[i].type;
    }

    if (k > 0) {
        // The list may already have entries, so link in front of them.
        e820_ext->next = setup_header->setup_data;
        e820_ext->type = SETUP_E820_EXT;
        e820_ext->len = k * sizeof(struct boot_e820_entry);
        setup_header->setup_data = (uintptr_t)e820_ext;
    }

    ///////////////////////////////////////
    // Spin up
    ///////////////////////////////////////

    // Linux enables x2APIC itself where it wants it, but a kernel built without
    // CONFIG_X86_X2APIC that is entered in x2APIC mode gives up the APIC
    // entirely, so hand over in xAPIC mode.
    if (rdmsr(0x1b) & (1 << 10)) {
        if (x2apic_disable()) {
            printv("linux: Firmware had x2APIC enabled, reverted to xAPIC mode\n");
        } else {
            printv("linux: Firmware has x2APIC enabled and it could not be disabled\n");
        }
    }

    // Taking over an IOMMU left enabled at entry arrived in 4.2 for VT-d and in
    // 4.14 for AMD-Vi. Leave it to newer kernels, which keeps the firmware's DMA
    // protection up across the handoff.
    if (linux_ver < LINUX_VER(4, 2)) {
        vtd_disable_all();
    }
    if (linux_ver < LINUX_VER(4, 14)) {
        amdvi_disable_all();
    }

    irq_flush_type = IRQ_PIC_ONLY_FLUSH;

#if defined (UEFI) && defined (__x86_64__)
    if (use_64_bit_proto == true && xlf_64bit_entry) {
        flush_irqs();
        linux_spinup64((void *)kernel_load_addr + 0x200, boot_params);
    }
#endif

#if defined (UEFI) && defined (__x86_64__)
    void *spinup_fn = spinup_tramp_low((void *)linux_spinup);
#else
    void *spinup_fn = (void *)linux_spinup;
#endif

    common_spinup(spinup_fn, 2, (uint32_t)kernel_load_addr,
                                (uint32_t)(uintptr_t)boot_params);
}

#endif
