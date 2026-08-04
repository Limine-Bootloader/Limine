#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include <e9print.h>
#include <flanterm.h>
#include <flanterm_backends/fb.h>

int memcmp(const void *, const void *, size_t);

#ifdef ENABLE_QEMU_SHUTDOWN
static inline void outw(uint16_t port, uint16_t value) {
    __asm volatile ("outw %%ax, %1"  : : "a" (value), "Nd" (port) : "memory");
}
#endif

__attribute__((section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

static void limine_main(void);

__attribute__((used, section(".limine_requests_start_marker")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_entry_point_request entry_point_request = {
    .id = LIMINE_ENTRY_POINT_REQUEST_ID,
    .revision = 0, .response = NULL,

    .entry = limine_main
};

__attribute__((section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_executable_cmdline_request executable_cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_firmware_type_request firmware_type_request = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_executable_file_request exec_file_request = {
    .id = LIMINE_EXECUTABLE_FILE_REQUEST_ID,
    .revision = 0, .response = NULL
};

struct limine_internal_module internal_module1 = {
    .path = "/boot/test.elf",
    .string = "First internal module"
};

struct limine_internal_module internal_module2 = {
    .path = "test.elf",
    .string = "Second internal module"
};

struct limine_internal_module internal_module3 = {
    .path = "./limine.conf",
    .string = "Third internal module"
    /*  gzip test depends on this name to find
        the original to compare against.  */
};

#ifdef ENABLE_GZIP_TEST
struct limine_internal_module internal_module4 = {
    .path = "./limine.conf.gz",
    .string = "gzip-compressed limine.conf",
    .flags = LIMINE_INTERNAL_MODULE_COMPRESSED
};
#endif

struct limine_internal_module *internal_modules[] = {
    &internal_module1,
    &internal_module2,
    &internal_module3,
#ifdef ENABLE_GZIP_TEST
    &internal_module4,
#endif
};

__attribute__((section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 1, .response = NULL,

    .internal_module_count = sizeof(internal_modules) / sizeof(internal_modules[0]),
    .internal_modules = internal_modules
};

__attribute__((section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_smbios_request smbios_request = {
    .id = LIMINE_SMBIOS_REQUEST_ID,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_efi_system_table_request est_request = {
    .id = LIMINE_EFI_SYSTEM_TABLE_REQUEST_ID,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_tpm_event_log_request tpm_event_log_request = {
    .id = LIMINE_TPM_EVENT_LOG_REQUEST_ID,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_efi_memmap_request efi_memmap_request = {
    .id = LIMINE_EFI_MEMMAP_REQUEST_ID,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_date_at_boot_request date_at_boot_request = {
    .id = LIMINE_DATE_AT_BOOT_REQUEST_ID,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_executable_address_request executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_mp_request _mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_dtb_request _dtb_request = {
    .id = LIMINE_DTB_REQUEST_ID,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_paging_mode_request _pm_request = {
    .id = LIMINE_PAGING_MODE_REQUEST_ID,
    .revision = 1, .response = NULL,
#if defined (__x86_64__)
    .mode = LIMINE_PAGING_MODE_X86_64_5LVL,
    .max_mode = LIMINE_PAGING_MODE_X86_64_5LVL,
    .min_mode = LIMINE_PAGING_MODE_X86_64_MIN
#elif defined (__aarch64__)
    .mode = LIMINE_PAGING_MODE_AARCH64_5LVL,
    .max_mode = LIMINE_PAGING_MODE_AARCH64_5LVL,
    .min_mode = LIMINE_PAGING_MODE_AARCH64_MIN
#elif defined (__riscv)
    .mode = LIMINE_PAGING_MODE_RISCV_SV57,
    .max_mode = LIMINE_PAGING_MODE_RISCV_SV57,
    .min_mode = LIMINE_PAGING_MODE_RISCV_MIN,
#elif defined (__loongarch__)
    .mode = LIMINE_PAGING_MODE_LOONGARCH_DEFAULT,
    .max_mode = LIMINE_PAGING_MODE_LOONGARCH_DEFAULT,
    .min_mode = LIMINE_PAGING_MODE_LOONGARCH_MIN
#endif
};

#ifdef __riscv
__attribute__((section(".limine_requests")))
static volatile struct limine_riscv_bsp_hartid_request _bsp_request = {
    .id = LIMINE_RISCV_BSP_HARTID_REQUEST_ID,
    .revision = 0, .response = NULL,
};
#endif

__attribute__((section(".limine_requests")))
static volatile struct limine_tsc_frequency_request tsc_freq_request = {
    .id = LIMINE_TSC_FREQUENCY_REQUEST_ID,
    .revision = 0, .response = NULL,
};

__attribute__((section(".limine_requests")))
static volatile struct limine_bootloader_performance_request _perf_request = {
    .id = LIMINE_BOOTLOADER_PERFORMANCE_REQUEST_ID,
    .revision = 0, .response = NULL,
};

__attribute__((section(".limine_requests")))
static volatile struct limine_flanterm_fb_init_params_request fip_request = {
    .id = LIMINE_FLANTERM_FB_INIT_PARAMS_REQUEST_ID,
    .revision = 0, .response = NULL,
};

__attribute__((used, section(".limine_requests_end_marker")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

static char *get_memmap_type(uint64_t type) {
    switch (type) {
        case LIMINE_MEMMAP_USABLE:
            return "Usable";
        case LIMINE_MEMMAP_RESERVED:
            return "Reserved";
        case LIMINE_MEMMAP_RESERVED_MAPPED:
            return "Reserved (Mapped)";
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
            return "ACPI reclaimable";
        case LIMINE_MEMMAP_ACPI_NVS:
            return "ACPI NVS";
        case LIMINE_MEMMAP_BAD_MEMORY:
            return "Bad memory";
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
            return "Bootloader reclaimable";
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
            return "Executable and modules";
        case LIMINE_MEMMAP_FRAMEBUFFER:
            return "Framebuffer";
        default:
            return "???";
    }
}

static char *firmware_type_str(uint64_t t) {
    switch (t) {
        case LIMINE_FIRMWARE_TYPE_X86BIOS:
            return "x86 BIOS";
        case LIMINE_FIRMWARE_TYPE_EFI32:
            return "32-bit EFI";
        case LIMINE_FIRMWARE_TYPE_EFI64:
            return "64-bit EFI";
        default:
            return "???";
    }
}

static void print_file(struct limine_file *file) {
    printf("File->Revision: %lu\n", file->revision);
    printf("File->Address: %p\n", file->address);
    printf("File->Size: %#lx\n", file->size);
    printf("File->Path: %s\n", file->path);
    printf("File->String: %s\n", file->string);
    printf("File->MediaType: %d\n", file->media_type);
    printf("File->PartIndex: %d\n", file->partition_index);
    printf("File->TFTPIP: %d.%d.%d.%d\n",
              file->tftp_ipv4[0],
              file->tftp_ipv4[1],
              file->tftp_ipv4[2],
              file->tftp_ipv4[3]);
    printf("File->TFTPPort: %d\n", file->tftp_port);
    printf("File->MBRDiskId: %#x\n", file->mbr_disk_id);
    printf("File->GPTDiskUUID: %#x-%#x-%#x-%#lx\n",
              file->gpt_disk_uuid.a,
              file->gpt_disk_uuid.b,
              file->gpt_disk_uuid.c,
              *(uint64_t *)file->gpt_disk_uuid.d);
    printf("File->GPTPartUUID: %#x-%#x-%#x-%#lx\n",
              file->gpt_part_uuid.a,
              file->gpt_part_uuid.b,
              file->gpt_part_uuid.c,
              *(uint64_t *)file->gpt_part_uuid.d);
    printf("File->PartUUID: %#x-%#x-%#x-%#lx\n",
              file->part_uuid.a,
              file->part_uuid.b,
              file->part_uuid.c,
              *(uint64_t *)file->part_uuid.d);
}

uint32_t ctr = 0;

void ap_entry(struct limine_mp_info *info) {
    printf("Hello from AP!\n");

#if defined (__x86_64__)
    printf("My LAPIC ID: %#x\n", info->lapic_id);
#elif defined (__aarch64__)
    printf("My MPIDR: %#lx\n", info->mpidr);
#elif defined (__riscv)
    printf("My Hart ID: %#lx\n", info->hartid);
#elif defined (__loongarch__)
    printf("My Phys ID: %#lx\n", info->phys_id);
#endif

    __atomic_fetch_add(&ctr, 1, __ATOMIC_SEQ_CST);

    while (1);
}

#define FEAT_START do {
#define FEAT_END } while (0);

extern char executable_start[];

struct flanterm_context *ft_ctx = NULL;

static uint8_t alloc_pool[16 * 1024 * 1024];
static size_t alloc_off = 0;

static void *simple_malloc(size_t size) {
    size = (size + 15) & ~(size_t)15;
    if (alloc_off + size > sizeof(alloc_pool)) {
        return NULL;
    }
    void *p = &alloc_pool[alloc_off];
    alloc_off += size;
    return p;
}

static void simple_free(void *ptr, size_t size) {
    (void)ptr;
    (void)size;
}

static void limine_main(void) {
    printf("\nWe're alive\n");

    if (LIMINE_LOADED_BASE_REVISION_VALID(limine_base_revision) == true) {
        printf("Bootloader has loaded us using base revision %lu\n",
                  LIMINE_LOADED_BASE_REVISION(limine_base_revision));
    }

    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
        printf("Limine base revision not supported\n");
        for (;;);
    }

    printf("\n");

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];

    struct limine_flanterm_fb_init_params *fip = NULL;
    if (fip_request.response != NULL && fip_request.response->entry_count > 0) {
        fip = fip_request.response->entries[0];
    }

    if (fip != NULL) {
        ft_ctx = flanterm_fb_init(
            simple_malloc,
            simple_free,
            fb->address, fb->width, fb->height, fb->pitch,
            fb->red_mask_size, fb->red_mask_shift,
            fb->green_mask_size, fb->green_mask_shift,
            fb->blue_mask_size, fb->blue_mask_shift,
            fip->canvas,
            fip->ansi_colours, fip->ansi_bright_colours,
            &fip->default_bg, &fip->default_fg,
            &fip->default_bg_bright, &fip->default_fg_bright,
            fip->font, fip->font_width, fip->font_height, fip->font_spacing,
            fip->font_scale_x, fip->font_scale_y,
            fip->margin,
            fip->rotation
        );
    } else {
        ft_ctx = flanterm_fb_init(
            NULL,
            NULL,
            fb->address, fb->width, fb->height, fb->pitch,
            fb->red_mask_size, fb->red_mask_shift,
            fb->green_mask_size, fb->green_mask_shift,
            fb->blue_mask_size, fb->blue_mask_shift,
            NULL,
            NULL, NULL,
            NULL, NULL,
            NULL, NULL,
            NULL, 0, 0, 1,
            0, 0,
            0,
            FLANTERM_FB_ROTATE_0
        );
    }

    uint64_t executable_slide = (uint64_t)executable_start - 0xffffffff80000000;

    printf("Executable start: %p\n", executable_start);
    printf("Executable slide: %#lx\n", executable_slide);

FEAT_START
    printf("\n");
    if (bootloader_info_request.response == NULL) {
        printf("Bootloader info not passed\n");
        break;
    }
    struct limine_bootloader_info_response *bootloader_info_response = bootloader_info_request.response;
    printf("Bootloader info feature, revision %lu\n", bootloader_info_response->revision);
    printf("Bootloader name: %s\n", bootloader_info_response->name);
    printf("Bootloader version: %s\n", bootloader_info_response->version);
FEAT_END

FEAT_START
    printf("\n");
    if (executable_cmdline_request.response == NULL) {
        printf("Executable command line not passed\n");
        break;
    }
    struct limine_executable_cmdline_response *executable_cmdline_response = executable_cmdline_request.response;
    printf("Executable command line feature, revision %lu\n", executable_cmdline_response->revision);
    printf("Command line: %s\n", executable_cmdline_response->cmdline);
FEAT_END

FEAT_START
    printf("\n");
    if (firmware_type_request.response == NULL) {
        printf("Firmware type not passed\n");
        break;
    }
    struct limine_firmware_type_response *firmware_type_response = firmware_type_request.response;
    printf("Firmware type feature, revision %lu\n", firmware_type_response->revision);
    printf("Firmware type: %s\n", firmware_type_str(firmware_type_response->firmware_type));
FEAT_END

FEAT_START
    printf("\n");
    if (executable_address_request.response == NULL) {
        printf("Executable address not passed\n");
        break;
    }
    struct limine_executable_address_response *exec_addr_response = executable_address_request.response;
    printf("Executable address feature, revision %lu\n", exec_addr_response->revision);
    printf("Physical base: %#lx\n", exec_addr_response->physical_base);
    printf("Virtual base: %#lx\n", exec_addr_response->virtual_base);
FEAT_END

FEAT_START
    printf("\n");
    if (hhdm_request.response == NULL) {
        printf("HHDM not passed\n");
        break;
    }
    struct limine_hhdm_response *hhdm_response = hhdm_request.response;
    printf("HHDM feature, revision %lu\n", hhdm_response->revision);
    printf("Higher half direct map at: %#lx\n", hhdm_response->offset);
FEAT_END

FEAT_START
    printf("\n");
    if (memmap_request.response == NULL) {
        printf("Memory map not passed\n");
        break;
    }
    struct limine_memmap_response *memmap_response = memmap_request.response;
    printf("Memory map feature, revision %lu\n", memmap_response->revision);
    printf("%lu memory map entries\n", memmap_response->entry_count);
    for (size_t i = 0; i < memmap_response->entry_count; i++) {
        struct limine_memmap_entry *e = memmap_response->entries[i];
        printf("%#lx->%#lx %s\n", e->base, e->base + e->length, get_memmap_type(e->type));
    }
FEAT_END

FEAT_START
    printf("\n");
    if (framebuffer_request.response == NULL) {
        printf("Framebuffer not passed\n");
        break;
    }
    struct limine_framebuffer_response *fb_response = framebuffer_request.response;
    printf("Framebuffers feature, revision %lu\n", fb_response->revision);
    printf("%lu framebuffer(s)\n", fb_response->framebuffer_count);
    for (size_t i = 0; i < fb_response->framebuffer_count; i++) {
        struct limine_framebuffer *fb = fb_response->framebuffers[i];
        printf("Address: %p\n", fb->address);
        printf("Width: %lu\n", fb->width);
        printf("Height: %lu\n", fb->height);
        printf("Pitch: %lu\n", fb->pitch);
        printf("BPP: %d\n", fb->bpp);
        printf("Memory model: %d\n", fb->memory_model);
        printf("Red mask size: %d\n", fb->red_mask_size);
        printf("Red mask shift: %d\n", fb->red_mask_shift);
        printf("Green mask size: %d\n", fb->green_mask_size);
        printf("Green mask shift: %d\n", fb->green_mask_shift);
        printf("Blue mask size: %d\n", fb->blue_mask_size);
        printf("Blue mask shift: %d\n", fb->blue_mask_shift);
        printf("EDID size: %lu\n", fb->edid_size);
        printf("EDID at: %p\n", fb->edid);
        printf("Video modes:\n");
        for (size_t j = 0; j < fb->mode_count; j++) {
            printf("  %lux%lux%lu\n", (unsigned long)fb->modes[j]->width, (unsigned long)fb->modes[j]->height, (unsigned long)fb->modes[j]->bpp);
        }
    }
FEAT_END

FEAT_START
    printf("\n");
    if (fip_request.response == NULL) {
        printf("Flanterm FB init params not passed\n");
        break;
    }
    struct limine_flanterm_fb_init_params_response *fip_response = fip_request.response;
    printf("Flanterm FB init params feature, revision %lu\n", fip_response->revision);
    printf("%lu entry/entries\n", fip_response->entry_count);
    for (size_t i = 0; i < fip_response->entry_count; i++) {
        struct limine_flanterm_fb_init_params *p = fip_response->entries[i];
        printf("--- Entry %lu ---\n", i);
        printf("Canvas: %#lx (size: %#lx)\n", (unsigned long)p->canvas, p->canvas_size);
        printf("Default BG: %#x, FG: %#x\n", p->default_bg, p->default_fg);
        printf("Default BG bright: %#x, FG bright: %#x\n", p->default_bg_bright, p->default_fg_bright);
        printf("ANSI colours: %#x %#x %#x %#x %#x %#x %#x %#x\n",
            p->ansi_colours[0], p->ansi_colours[1], p->ansi_colours[2], p->ansi_colours[3],
            p->ansi_colours[4], p->ansi_colours[5], p->ansi_colours[6], p->ansi_colours[7]);
        printf("ANSI bright: %#x %#x %#x %#x %#x %#x %#x %#x\n",
            p->ansi_bright_colours[0], p->ansi_bright_colours[1], p->ansi_bright_colours[2], p->ansi_bright_colours[3],
            p->ansi_bright_colours[4], p->ansi_bright_colours[5], p->ansi_bright_colours[6], p->ansi_bright_colours[7]);
        printf("Font: %#lx (%lux%lu)\n", (unsigned long)p->font, p->font_width, p->font_height);
        printf("Font spacing: %lu, scale: %lux%lu\n", p->font_spacing, p->font_scale_x, p->font_scale_y);
        printf("Margin: %lu, Rotation: %lu\n", p->margin, p->rotation);
    }
FEAT_END

FEAT_START
    printf("\n");
    if (exec_file_request.response == NULL) {
        printf("Executable file not passed\n");
        break;
    }
    struct limine_executable_file_response *exec_file_response = exec_file_request.response;
    printf("Executable file feature, revision %lu\n", exec_file_response->revision);
    print_file(exec_file_response->executable_file);
FEAT_END

FEAT_START
    printf("\n");
    if (module_request.response == NULL) {
        printf("Modules not passed\n");
        break;
    }
    struct limine_module_response *module_response = module_request.response;
    printf("Modules feature, revision %lu\n", module_response->revision);
    printf("%lu module(s)\n", module_response->module_count);
    for (size_t i = 0; i < module_response->module_count; i++) {
        struct limine_file *f = module_response->modules[i];
        printf("---\n");
        print_file(f);
    }

#ifdef ENABLE_GZIP_TEST
    /*  Gzip decompression test: compare internal_module3 (plain limine.conf)
        against internal_module4 (limine.conf.gz, decompressed by bootloader).  */
    {
        struct limine_file *plain = NULL, *decompressed = NULL;
        for (size_t i = 0; i < module_response->module_count; i++) {
            struct limine_file *f = module_response->modules[i];
            if (f->string != NULL) {
                /*  Match by the module string we assigned.  */
                bool is_third = f->string[0] == 'T' && f->string[1] == 'h'
                             && f->string[2] == 'i' && f->string[3] == 'r'
                             && f->string[4] == 'd';
                bool is_gz    = f->string[0] == 'g' && f->string[1] == 'z';
                if (is_third) plain = f;
                if (is_gz)    decompressed = f;
            }
        }
        if (plain == NULL) {
            printf("gzip: FAIL (plain module not found)\n");
        } else if (decompressed == NULL) {
            printf("gzip: FAIL (decompressed module not found)\n");
        } else if (plain->size != decompressed->size) {
            printf("gzip: FAIL (size mismatch: plain=%#x, decompressed=%#x)\n",
                      plain->size, decompressed->size);
        } else if (memcmp(plain->address, decompressed->address, plain->size) != 0) {
            printf("gzip: FAIL (content mismatch, size=%#x)\n", plain->size);
        } else {
            printf("gzip: pass (size=%#x)\n", plain->size);
        }
    }
#endif
FEAT_END

FEAT_START
    printf("\n");
    if (rsdp_request.response == NULL) {
        printf("RSDP not passed\n");
        break;
    }
    struct limine_rsdp_response *rsdp_response = rsdp_request.response;
    printf("RSDP feature, revision %lu\n", rsdp_response->revision);
    printf("RSDP at: %p\n", rsdp_response->address);
FEAT_END

FEAT_START
    printf("\n");
    if (smbios_request.response == NULL) {
        printf("SMBIOS not passed\n");
        break;
    }
    struct limine_smbios_response *smbios_response = smbios_request.response;
    printf("SMBIOS feature, revision %lu\n", smbios_response->revision);
    printf("SMBIOS 32-bit entry at: %p\n", smbios_response->entry_32);
    printf("SMBIOS 64-bit entry at: %p\n", smbios_response->entry_64);
FEAT_END

FEAT_START
    printf("\n");
    if (est_request.response == NULL) {
        printf("EFI system table not passed\n");
        break;
    }
    struct limine_efi_system_table_response *est_response = est_request.response;
    printf("EFI system table feature, revision %lu\n", est_response->revision);
    printf("EFI system table at: %p\n", est_response->address);
FEAT_END

FEAT_START
    printf("\n");
    if (tpm_event_log_request.response == NULL) {
        printf("TPM event log not passed\n");
        break;
    }
    struct limine_tpm_event_log_response *tpm_event_log_response = tpm_event_log_request.response;
    printf("TPM event log feature, revision %lu\n", tpm_event_log_response->revision);
    printf("Format: %lu (TCG_%s)\n", tpm_event_log_response->format,
              tpm_event_log_response->format == LIMINE_TPM_EVENT_LOG_FORMAT_TCG_2 ? "2" : "1.2");
    printf("Size: %#lx bytes\n", tpm_event_log_response->size);
    printf("Address: %p\n", tpm_event_log_response->address);
FEAT_END

FEAT_START
    printf("\n");
    if (efi_memmap_request.response == NULL) {
        printf("EFI memory map not passed\n");
        break;
    }
    struct limine_efi_memmap_response *efi_memmap_response = efi_memmap_request.response;
    printf("EFI memory map feature, revision %lu\n", efi_memmap_response->revision);
    printf("EFI memory map at: %p\n", efi_memmap_response->memmap);
    printf("EFI memory map size: %#lx\n", efi_memmap_response->memmap_size);
    printf("EFI memory descriptor size: %#lx\n", efi_memmap_response->desc_size);
    printf("EFI memory descriptor version: %lu\n", efi_memmap_response->desc_version);
FEAT_END

FEAT_START
    printf("\n");
    if (date_at_boot_request.response == NULL) {
        printf("Boot time not passed\n");
        break;
    }
    struct limine_date_at_boot_response *date_at_boot_response = date_at_boot_request.response;
    printf("Date at boot feature, revision %lu\n", date_at_boot_response->revision);
    printf("Timestamp: %lu\n", date_at_boot_response->timestamp);
FEAT_END

FEAT_START
    printf("\n");
    if (_mp_request.response == NULL) {
        printf("MP info not passed\n");
        break;
    }
    struct limine_mp_response *mp_response = _mp_request.response;
    printf("MP feature, revision %lu\n", mp_response->revision);
    printf("Flags: %#lx\n", (unsigned long)mp_response->flags);
#if defined (__x86_64__)
    printf("BSP LAPIC ID: %#x\n", mp_response->bsp_lapic_id);
#elif defined (__aarch64__)
    printf("BSP MPIDR: %#lx\n", mp_response->bsp_mpidr);
#elif defined (__riscv)
    printf("BSP Hart ID: %#lx\n", mp_response->bsp_hartid);
#elif defined (__loongarch__)
    printf("BSP Phys ID: %#lx\n", mp_response->bsp_phys_id);
#endif
    printf("CPU count: %lu\n", mp_response->cpu_count);
    for (size_t i = 0; i < mp_response->cpu_count; i++) {
        struct limine_mp_info *cpu = mp_response->cpus[i];
        printf("Processor ID: %#lx\n", (unsigned long)cpu->processor_id);
#if defined (__x86_64__)
        printf("LAPIC ID: %#x\n", cpu->lapic_id);
#elif defined (__aarch64__)
        printf("MPIDR: %#lx\n", cpu->mpidr);
#elif defined (__riscv)
        printf("Hart ID: %#lx\n", cpu->hartid);
#elif defined (__loongarch__)
        printf("Phys ID: %#lx\n", cpu->phys_id);
#endif


#if defined (__x86_64__)
        if (cpu->lapic_id != mp_response->bsp_lapic_id) {
#elif defined (__aarch64__)
        if (cpu->mpidr != mp_response->bsp_mpidr) {
#elif defined (__riscv)
        if (cpu->hartid != mp_response->bsp_hartid) {
#elif defined (__loongarch__)
        if (cpu->phys_id != mp_response->bsp_phys_id) {
#endif
            uint32_t old_ctr = __atomic_load_n(&ctr, __ATOMIC_SEQ_CST);

            __atomic_store_n(&cpu->goto_address, ap_entry, __ATOMIC_SEQ_CST);

            while (__atomic_load_n(&ctr, __ATOMIC_SEQ_CST) == old_ctr)
                ;
        }
    }
FEAT_END

FEAT_START
    printf("\n");
    if (_dtb_request.response == NULL) {
        printf("Device tree blob not passed\n");
        break;
    }
    struct limine_dtb_response *dtb_response = _dtb_request.response;
    printf("Device tree blob feature, revision %lu\n", dtb_response->revision);
    printf("Device tree blob pointer: %p\n", dtb_response->dtb_ptr);
    uint32_t dtb_magic = *(uint32_t*)dtb_response->dtb_ptr;
    printf("Device tree header magic: %#x\n", dtb_magic);
FEAT_END

FEAT_START
    printf("\n");
    if (_pm_request.response == NULL) {
        printf("Paging mode not passed\n");
        break;
    }
    struct limine_paging_mode_response *pm_response = _pm_request.response;
    printf("Paging mode feature, revision %lu\n", pm_response->revision);
    printf("  mode: %lu\n", pm_response->mode);
FEAT_END

#if defined (__riscv)
FEAT_START
    printf("\n");
    struct limine_riscv_bsp_hartid_response *bsp_response = _bsp_request.response;
    if (bsp_response == NULL) {
        printf("RISC-V BSP Hart ID was not passed\n");
        break;
    }
    printf("RISC-V BSP Hart ID: %#lx\n", bsp_response->bsp_hartid);
FEAT_END
#endif

FEAT_START
    printf("\n");
    struct limine_tsc_frequency_response *tsc_freq_response = tsc_freq_request.response;
    if (tsc_freq_response == NULL) {
        printf("TSC frequency not passed\n");
        break;
    }
    printf("TSC frequency feature, revision %lu\n", tsc_freq_response->revision);
    printf("Frequency: %lu Hz\n", tsc_freq_response->frequency);
FEAT_END

FEAT_START
    printf("\n");
    struct limine_bootloader_performance_response *perf_response = _perf_request.response;
    if (perf_response == NULL) {
        printf("Bootloader performance not passed\n");
        break;
    }
    printf("Bootloader performance feature, revision %lu\n", perf_response->revision);
    printf("Reset time: %lu usec\n", perf_response->reset_usec);
    printf("Init time: %lu usec\n", perf_response->init_usec);
    printf("Exec time: %lu usec\n", perf_response->exec_usec);
FEAT_END

#ifdef ENABLE_QEMU_SHUTDOWN
    outw(0x604, 0x2000); /*  QEMU-specific shutdown, used by automated tests.  */
#endif
    for (;;);
}
