#if defined(__riscv) || defined(__aarch64__) || defined(__loongarch__)

#include <stdint.h>
#include <stddef.h>
#include <stdnoreturn.h>
#include <protos/linux.h>
#include <fs/file.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/term.h>
#include <lib/config.h>
#include <lib/print.h>
#include <lib/uri.h>
#include <mm/pmm.h>
#include <sys/idt.h>
#include <lib/fb.h>
#include <lib/acpi.h>
#include <lib/fdt.h>
#include <libfdt.h>

// The following definitions and struct were copied and adapted from Linux
// kernel headers released under GPL-2.0 WITH Linux-syscall-note
// allowing their inclusion in non GPL compliant code.

#if defined(__riscv) || defined(__aarch64__)
struct linux_header {
    uint32_t code0;
    uint32_t code1;
    uint64_t text_offset;
    uint64_t image_size;
    uint64_t flags;
    uint32_t version;
    uint32_t res1;
    uint64_t res2;
    uint64_t res3;          // originally 'magic' field, deprecated
    uint32_t magic2;
    uint32_t res4;
} __attribute__((packed));
#elif defined(__loongarch__)
struct linux_header {
    uint32_t mz;
    uint32_t res0;
    uint64_t kernel_entry;
    uint64_t image_size;
    uint64_t load_offset;
    uint64_t res1;
    uint64_t res2;
    uint64_t res3;
    uint32_t magic2;       // LINUX_PE_MAGIC
    uint32_t pe_offset;
} __attribute__((packed));
#else
#error "Unknown architecture"
#endif

struct linux_efi_memreserve {
    int size;
    int count;
    uint64_t next;
};

struct linux_efi_boot_memmap {
    UINTN    map_size;
    UINTN    desc_size;
    uint32_t desc_ver;
    UINTN    map_key;
    UINTN    buff_size;
    EFI_MEMORY_DESCRIPTOR descs[];
};

struct linux_efi_initrd {
    UINTN base;
    UINTN size;
};

// End of Linux code

struct boot_param {
    void *kernel_base;
    size_t kernel_size;
    void *module_base;
    size_t module_size;
    char *cmdline;
    void *dtb;
    struct linux_efi_boot_memmap *memmap;
};

#if defined(__riscv)
#define LINUX_HEADER_MAGIC2             0x05435352
#define LINUX_HEADER_MAJOR_VER(ver)     (((ver) >> 16) & 0xffff)
#define LINUX_HEADER_MINOR_VER(ver)     (((ver) >> 0)  & 0xffff)
#elif defined(__aarch64__)
#define LINUX_HEADER_MAGIC2             0x644d5241
#elif defined(__loongarch__)
#define LINUX_HEADER_MAGIC2             0x818223cd
#endif

const char *verify_kernel(struct boot_param *p) {
    struct linux_header *header = p->kernel_base;

    if (header->magic2 != LINUX_HEADER_MAGIC2) {
        return "kernel header magic does not match";
    }

    // riscv-specific version requirements
#if defined(__riscv)
    printv("linux: boot protocol version %d.%d\n",
           LINUX_HEADER_MAJOR_VER(header->version),
           LINUX_HEADER_MINOR_VER(header->version));
    if (LINUX_HEADER_MAJOR_VER(header->version) == 0
     && LINUX_HEADER_MINOR_VER(header->version) < 2) {
        return "linux: protocols < 0.2 are not supported";
    }
#endif

    return NULL;
}

void load_module(struct boot_param *p, char *config) {
    char *module_path = config_get_value(config, 0, "MODULE_PATH");
    if (module_path) {
        print("linux: Loading module `%#`...\n", module_path);

        struct file_handle *module_file = uri_open(module_path);
        if (!module_file) {
            panic(true, "linux: failed to open module `%s`. Is the path correct?", module_path);
        }

        p->module_size = module_file->size;
        p->module_base = ext_mem_alloc_type_aligned(
                        ALIGN_UP(p->module_size, 4096),
                        MEMMAP_KERNEL_AND_MODULES, 4096);

        fread(module_file, p->module_base, 0, p->module_size);
        fclose(module_file);
        printv("linux: loaded module `%s` at %x, size %u\n", module_path, p->module_base, p->module_size);
    }
}

void prepare_device_tree_blob(struct boot_param *p) {
    void *dtb = p->dtb;
    int ret;

    // Delete all /memory@... nodes. Linux will use the given UEFI memory map
    // instead.
    while (true) {
        int offset = fdt_subnode_offset_namelen(dtb, 0, "memory@", 7);

        if (offset == -FDT_ERR_NOTFOUND) {
            break;
        }

        if (offset < 0) {
            panic(true, "linux: failed to find node: '%s'", fdt_strerror(offset));
        }

        ret = fdt_del_node(dtb, offset);
        if (ret < 0) {
            panic(true, "linux: failed to delete memory node: '%s'", fdt_strerror(ret));
        }
    }

    if (p->module_base) {
        ret = fdt_set_chosen_uint64(dtb, "linux,initrd-start", (uint64_t)p->module_base);
        if (ret < 0) {
            panic(true, "linux: cannot set initrd parameter: '%s'", fdt_strerror(ret));
        }

        ret = fdt_set_chosen_uint64(dtb, "linux,initrd-end", (uint64_t)(p->module_base + p->module_size));
        if (ret < 0) {
            panic(true, "linux: cannot set initrd parameter: '%s'", fdt_strerror(ret));
        }
    }

    // Set the kernel command line arguments.
    ret = fdt_set_chosen_string(dtb, "bootargs", p->cmdline);
    if (ret < 0) {
        panic(true, "linux: failed to set bootargs: '%s'", fdt_strerror(ret));
    }

    // Tell Linux about the UEFI memory map and system table.
    ret = fdt_set_chosen_uint64(dtb, "linux,uefi-system-table", (uint64_t)gST);
    if (ret < 0) {
        panic(true, "linux: failed to set UEFI system table pointer: '%s'", fdt_strerror(ret));
    }

    // This property is not required by mainline Linux, but is required by
    // Debian (and derivative) kernels, because Debian has a patch that adds
    // this flag, and the existing logic that deals with it will just outright
    // fail if any of the properties is missing.  We don't care about Debian's
    // hardening or whatever, so just always report that secure boot is off.
    ret = fdt_set_chosen_uint32(dtb, "linux,uefi-secure-boot", 0);
    if (ret < 0) {
        panic(true, "linux: failed to set UEFI secure boot state: '%s'", fdt_strerror(ret));
    }
}

void add_framebuffer(struct fb_info *fb) {
    struct screen_info *screen_info = ext_mem_alloc(sizeof(struct screen_info));

    screen_info->capabilities   = VIDEO_CAPABILITY_64BIT_BASE | VIDEO_CAPABILITY_SKIP_QUIRKS;
    screen_info->flags          = VIDEO_FLAGS_NOCURSOR;
    screen_info->lfb_base       = (uint32_t)fb->framebuffer_addr;
    screen_info->ext_lfb_base   = (uint32_t)(fb->framebuffer_addr >> 32);
    screen_info->lfb_size       = fb->framebuffer_pitch * fb->framebuffer_height;
    screen_info->lfb_width      = fb->framebuffer_width;
    screen_info->lfb_height     = fb->framebuffer_height;
    screen_info->lfb_depth      = fb->framebuffer_bpp;
    screen_info->lfb_linelength = fb->framebuffer_pitch;
    screen_info->red_size       = fb->red_mask_size;
    screen_info->red_pos        = fb->red_mask_shift;
    screen_info->green_size     = fb->green_mask_size;
    screen_info->green_pos      = fb->green_mask_shift;
    screen_info->blue_size      = fb->blue_mask_size;
    screen_info->blue_pos       = fb->blue_mask_shift;

    screen_info->orig_video_isVGA = VIDEO_TYPE_EFI;

    EFI_GUID screen_info_table_guid = {0xe03fc20a, 0x85dc, 0x406e, {0xb9, 0x0e, 0x4a, 0xb5, 0x02, 0x37, 0x1d, 0x95}};
    EFI_STATUS ret = gBS->InstallConfigurationTable(&screen_info_table_guid, screen_info);

    if (ret != EFI_SUCCESS) {
        panic(true, "linux: failed to install screen info configuration table: '%x'", ret);
    }
}

void prepare_efi_tables(struct boot_param *p, char *config) {
    (void)p;
    EFI_STATUS ret = 0;

    {
        size_t req_width = 0, req_height = 0, req_bpp = 0;

        char *resolution = config_get_value(config, 0, "RESOLUTION");
        if (resolution != NULL) {
            parse_resolution(&req_width, &req_height, &req_bpp, resolution);
        }

        struct fb_info *fbs;
        size_t fbs_count;

        term_notready();

        fb_init(&fbs, &fbs_count, req_width, req_height, req_bpp);

        // TODO(qookie): Let the user pick a framebuffer if there's > 1
        if (fbs_count > 0) {
            add_framebuffer(&fbs[0]);
        }
    }


    {
        struct linux_efi_memreserve *rsv = ext_mem_alloc(sizeof(struct linux_efi_memreserve));

        rsv->size = 0;
        rsv->count = 0;
        rsv->next = 0;

        EFI_GUID memreserve_table_guid = {0x888eb0c6, 0x8ede, 0x4ff5, {0xa8, 0xf0, 0x9a, 0xee, 0x5c, 0xb9, 0x77, 0xc2}};
        ret = gBS->InstallConfigurationTable(&memreserve_table_guid, rsv);

        if (ret != EFI_SUCCESS) {
            panic(true, "linux: failed to install memory reservation configuration table: '%x'", ret);
        }
    }

    if (p->module_base) {
        struct linux_efi_initrd *initrd_table;

        ret = gBS->AllocatePool(EfiLoaderData, sizeof(*initrd_table), (void **)&initrd_table);
        if (ret != EFI_SUCCESS) {
            panic(true, "linux: failed to allocate Linux initrd table");
        }

        initrd_table->base = (UINTN)p->module_base;
        initrd_table->size = p->module_size;

        EFI_GUID initrd_table_guid = { 0x5568e427, 0x68fc, 0x4f3d, { 0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68}};
        ret = gBS->InstallConfigurationTable(&initrd_table_guid, initrd_table);
        if (ret != EFI_SUCCESS) {
            panic(true, "linux: failed to install initrd\n");
        }
    }

    {
        size_t buff_size = sizeof(struct linux_efi_boot_memmap) + efi_mmap_size + 4096;

        ret = gBS->AllocatePool(EfiLoaderData, buff_size, (void **)&p->memmap);
        if (ret != EFI_SUCCESS) {
            panic(true, "linux: failed to allocate UEFI memory map");
        }

        p->memmap->buff_size = buff_size;

        EFI_GUID memmap_table_guid = { 0x800f683f, 0xd08b, 0x423a, { 0xa2, 0x93, 0x96, 0x5c, 0x3c, 0x6f, 0xe2, 0xb4}};
        ret = gBS->InstallConfigurationTable(&memmap_table_guid, p->memmap);
        if (ret != EFI_SUCCESS) {
            panic(true, "linux: failed to install UEFI memory map");
        }
    }

    efi_exit_boot_services();
}

void prepare_mmap(struct boot_param *p) {
    {
        void *dtb = p->dtb;
        int ret = fdt_set_chosen_uint64(dtb, "linux,uefi-mmap-start", (uint64_t)efi_mmap);
        if (ret < 0) {
            panic(true, "linux: failed to set UEFI memory map pointer: '%s'", fdt_strerror(ret));
        }

        ret = fdt_set_chosen_uint32(dtb, "linux,uefi-mmap-size", efi_mmap_size);
        if (ret < 0) {
            panic(true, "linux: failed to set UEFI memory map size: '%s'", fdt_strerror(ret));
        }

        ret = fdt_set_chosen_uint32(dtb, "linux,uefi-mmap-desc-size", efi_desc_size);
        if (ret < 0) {
            panic(true, "linux: failed to set UEFI memory map descriptor size: '%s'", fdt_strerror(ret));
        }

        ret = fdt_set_chosen_uint32(dtb, "linux,uefi-mmap-desc-ver", efi_desc_ver);
        if (ret < 0) {
            panic(true, "linux: failed to set UEFI memory map descriptor version: '%s'", fdt_strerror(ret));
        }
    }

    p->memmap->map_size  = efi_mmap_size;
    p->memmap->desc_size = efi_desc_size;
    p->memmap->desc_ver  = efi_desc_ver;
    p->memmap->map_key   = efi_mmap_key;

    size_t efi_mmap_entry_count = efi_mmap_size / efi_desc_size;
    for (size_t i = 0; i < efi_mmap_entry_count; i++) {
        EFI_MEMORY_DESCRIPTOR *entry = (void *)efi_mmap + i * efi_desc_size;

        if (entry->Attribute & EFI_MEMORY_RUNTIME) {
	    // LoongArch kernel requires the virtual address stays in the
	    // privileged, direct-mapped window
	    #if defined(__loongarch__)
       	        entry->VirtualStart = entry->PhysicalStart | (0x8ULL << 60);
            #else
	        entry->VirtualStart = entry->PhysicalStart;
	    #endif
        }
    }

    memcpy(&p->memmap->descs, efi_mmap, efi_mmap_size);

    EFI_STATUS status = gRT->SetVirtualAddressMap(efi_mmap_size, efi_desc_size, efi_desc_ver, efi_mmap);
    if (status != EFI_SUCCESS) {
        panic(false, "linux: failed to set UEFI virtual address map: '%x'", status);
    }
}

noreturn void jump_to_kernel(struct boot_param *p) {
#if defined(__riscv)
    printv("linux: bsp hart %d, device tree blob at %x\n", bsp_hartid, p->dtb);

    void (*kernel_entry)(uint64_t hartid, uint64_t dtb) = p->kernel_base;
    asm ("csrci   sstatus, 0x2\n\t"
         "csrw    sie, zero\n\t");
    kernel_entry(bsp_hartid, (uint64_t)p->dtb);
#elif defined(__aarch64__)
    printv("linux: device tree blob at %x\n", p->dtb);

    void (*kernel_entry)(uint64_t dtb, uint64_t res0, uint64_t res1, uint64_t res2) = p->kernel_base;
    asm ("msr daifset, 0xF");
    kernel_entry((uint64_t)p->dtb, 0, 0, 0);
#elif defined(__loongarch__)
// LoongArch kernel used to store virtual address in header.kernel_entry
// clearing the high 16bits ensures compatibility
#define TO_PHYS(addr) ((addr) & ((1ULL << 48) - 1))
#define CSR_DMW_PLV0  1ULL
#define CSR_DMW0_VSEG 0x8000ULL
#define CSR_DMW0_BASE (CSR_DMW0_VSEG << 48)
#define CSR_DMW0_INIT (CSR_DMW0_BASE | CSR_DMW_PLV0)
#define CSR_DMW1_MAT  (1 << 4)
#define CSR_DMW1_VSEG 0x9000ULL
#define CSR_DMW1_BASE (CSR_DMW1_VSEG << 48)
#define CSR_DMW1_INIT (CSR_DMW1_BASE | CSR_DMW1_MAT | CSR_DMW_PLV0)
#define CSR_DMW2_VSEG 0xa000ULL
#define CSR_DMW2_MAT  (2 << 4)
#define CSR_DMW2_BASE (CSR_DMW2_VSEG << 48)
#define CSR_DMW2_INIT (CSR_DMW2_BASE | CSR_DMW2_MAT | CSR_DMW_PLV0)
#define CSR_DMW3_INIT 0

    struct linux_header *header = p->kernel_base;
    void (*kernel_entry)(uint64_t efi_boot, uint64_t cmdline, uint64_t st);
    kernel_entry = p->kernel_base + (TO_PHYS(header->kernel_entry) - header->load_offset);

    asm volatile ("csrxchg $r0, %0, 0x0" :: "r" (0x4) : "memory");
    asm volatile ("csrwr   %0,  0x180"   :: "r" (CSR_DMW0_INIT) : "memory");
    asm volatile ("csrwr   %0,  0x181"   :: "r" (CSR_DMW1_INIT) : "memory");
    asm volatile ("csrwr   %0,  0x182"   :: "r" (CSR_DMW2_INIT) : "memory");
    asm volatile ("csrwr   %0,  0x183"   :: "r" (CSR_DMW3_INIT) : "memory");
    kernel_entry(1, (uint64_t)p->cmdline, (uint64_t)gST);
#endif
    __builtin_unreachable();
}

noreturn void linux_load(char *config, char *cmdline) {
    struct boot_param p;
    memset(&p, 0, sizeof(p));
    p.cmdline = cmdline;
    p.dtb = get_device_tree_blob(config, 0x1000);

    struct file_handle *kernel_file;

    char *kernel_path = config_get_value(config, 0, "PATH");
    if (kernel_path == NULL) {
        kernel_path = config_get_value(config, 0, "KERNEL_PATH");
    }
    if (kernel_path == NULL) {
        panic(true, "linux: Kernel path not specified");
    }

    print("linux: Loading kernel `%#`...\n", kernel_path);

    if ((kernel_file = uri_open(kernel_path)) == NULL) {
        panic(true, "linux: failed to open kernel `%s`. Is the path correct?", kernel_path);
    }

    p.kernel_size = kernel_file->size;
    p.kernel_base = ext_mem_alloc_type_aligned(
                ALIGN_UP(p.kernel_size, 4096),
                MEMMAP_KERNEL_AND_MODULES, 2 * 1024 * 1024);
    fread(kernel_file, p.kernel_base, 0, p.kernel_size);
    fclose(kernel_file);
    printv("linux: loaded kernel `%s` at %x, size %u\n", kernel_path, p.kernel_base, p.kernel_size);

    const char *reason = verify_kernel(&p);
    if (reason)
        panic(true, "linux: invalid kernel image: %s", reason);

    load_module(&p, config);

    prepare_device_tree_blob(&p);

    prepare_efi_tables(&p, config);

    prepare_mmap(&p);

    jump_to_kernel(&p);
}

#endif
