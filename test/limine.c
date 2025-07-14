#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include <e9print.h>
#include <flanterm.h>
#include <flanterm_backends/fb.h>

__attribute__((section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

static void limine_main(void);

__attribute__((used, section(".limine_requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_entry_point_request entry_point_request = {
    .id = LIMINE_ENTRY_POINT_REQUEST,
    .revision = 0, .response = NULL,

    .entry = limine_main
};

__attribute__((section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_executable_cmdline_request executable_cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_firmware_type_request firmware_type_request = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_executable_file_request exec_file_request = {
    .id = LIMINE_EXECUTABLE_FILE_REQUEST,
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
};

struct limine_internal_module *internal_modules[] = {
    &internal_module1,
    &internal_module2,
    &internal_module3
};

__attribute__((section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 1, .response = NULL,

    .internal_module_count = 3,
    .internal_modules = internal_modules
};

__attribute__((section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_smbios_request smbios_request = {
    .id = LIMINE_SMBIOS_REQUEST,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_efi_system_table_request est_request = {
    .id = LIMINE_EFI_SYSTEM_TABLE_REQUEST,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_efi_memmap_request efi_memmap_request = {
    .id = LIMINE_EFI_MEMMAP_REQUEST,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_date_at_boot_request date_at_boot_request = {
    .id = LIMINE_DATE_AT_BOOT_REQUEST,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_executable_address_request executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST,
    .revision = 0, .response = NULL
};

#ifndef __loongarch__
__attribute__((section(".limine_requests")))
static volatile struct limine_mp_request _mp_request = {
    .id = LIMINE_MP_REQUEST,
    .revision = 0, .response = NULL
};
#endif

__attribute__((section(".limine_requests")))
static volatile struct limine_dtb_request _dtb_request = {
    .id = LIMINE_DTB_REQUEST,
    .revision = 0, .response = NULL
};

__attribute__((section(".limine_requests")))
static volatile struct limine_paging_mode_request _pm_request = {
    .id = LIMINE_PAGING_MODE_REQUEST,
    .revision = 1, .response = NULL,
#if defined (__x86_64__)
    .mode = LIMINE_PAGING_MODE_X86_64_5LVL,
    .max_mode = LIMINE_PAGING_MODE_X86_64_5LVL,
#else
    .mode = LIMINE_PAGING_MODE_DEFAULT,
    .max_mode = LIMINE_PAGING_MODE_DEFAULT,
#endif
    .min_mode = LIMINE_PAGING_MODE_MIN
};

#ifdef __riscv
__attribute__((section(".limine_requests")))
static volatile struct limine_riscv_bsp_hartid_request _bsp_request = {
    .id = LIMINE_RISCV_BSP_HARTID_REQUEST,
    .revision = 0, .response = NULL,
};
#endif

__attribute__((section(".limine_requests")))
static volatile struct limine_bootloader_performance_request _perf_request = {
    .id = LIMINE_BOOTLOADER_PERFORMANCE_REQUEST,
    .revision = 0, .response = NULL,
};

__attribute__((used, section(".limine_requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

static char *get_memmap_type(uint64_t type) {
    switch (type) {
        case LIMINE_MEMMAP_USABLE:
            return "Usable";
        case LIMINE_MEMMAP_RESERVED:
            return "Reserved";
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
        case LIMINE_FIRMWARE_TYPE_UEFI32:
            return "32-bit UEFI";
        case LIMINE_FIRMWARE_TYPE_UEFI64:
            return "64-bit UEFI";
        default:
            return "???";
    }
}

static void print_file(struct limine_file *file) {
    e9_printf("File->Revision: %d", file->revision);
    e9_printf("File->Address: %x", file->address);
    e9_printf("File->Size: %x", file->size);
    e9_printf("File->Path: %s", file->path);
    e9_printf("File->String: %s", file->string);
    e9_printf("File->MediaType: %d", file->media_type);
    e9_printf("File->PartIndex: %d", file->partition_index);
    e9_printf("File->TFTPIP: %d.%d.%d.%d",
              (file->tftp_ip & (0xff << 0)) >> 0,
              (file->tftp_ip & (0xff << 8)) >> 8,
              (file->tftp_ip & (0xff << 16)) >> 16,
              (file->tftp_ip & (0xff << 24)) >> 24);
    e9_printf("File->TFTPPort: %d", file->tftp_port);
    e9_printf("File->MBRDiskId: %x", file->mbr_disk_id);
    e9_printf("File->GPTDiskUUID: %x-%x-%x-%x",
              file->gpt_disk_uuid.a,
              file->gpt_disk_uuid.b,
              file->gpt_disk_uuid.c,
              *(uint64_t *)file->gpt_disk_uuid.d);
    e9_printf("File->GPTPartUUID: %x-%x-%x-%x",
              file->gpt_part_uuid.a,
              file->gpt_part_uuid.b,
              file->gpt_part_uuid.c,
              *(uint64_t *)file->gpt_part_uuid.d);
    e9_printf("File->PartUUID: %x-%x-%x-%x",
              file->part_uuid.a,
              file->part_uuid.b,
              file->part_uuid.c,
              *(uint64_t *)file->part_uuid.d);
}

uint32_t ctr = 0;

void ap_entry(struct limine_mp_info *info) {
    e9_printf("Hello from AP!");

#if defined (__x86_64__)
    e9_printf("My LAPIC ID: %x", info->lapic_id);
#elif defined (__aarch64__)
    e9_printf("My MPIDR: %x", info->mpidr);
#elif defined (__riscv)
    e9_printf("My Hart ID: %x", info->hartid);
#elif defined (__loongarch__)
    (void)info;
#endif

    __atomic_fetch_add(&ctr, 1, __ATOMIC_SEQ_CST);

    while (1);
}

#define FEAT_START do {
#define FEAT_END } while (0);

extern char executable_start[];

struct flanterm_context *ft_ctx = NULL;

static void limine_main(void) {
    e9_printf("\nWe're alive");

    if (LIMINE_LOADED_BASE_REV_VALID == true) {
        e9_printf("Bootloader has loaded us using base revision %d", LIMINE_LOADED_BASE_REVISION);
    }

    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        e9_printf("Limine base revision not supported");
        for (;;);
    }

    e9_printf("");

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];

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
        0
    );

    uint64_t executable_slide = (uint64_t)executable_start - 0xffffffff80000000;

    e9_printf("Executable start: %x", executable_start);
    e9_printf("Executable slide: %x", executable_slide);

FEAT_START
    e9_printf("");
    if (bootloader_info_request.response == NULL) {
        e9_printf("Bootloader info not passed");
        break;
    }
    struct limine_bootloader_info_response *bootloader_info_response = bootloader_info_request.response;
    e9_printf("Bootloader info feature, revision %d", bootloader_info_response->revision);
    e9_printf("Bootloader name: %s", bootloader_info_response->name);
    e9_printf("Bootloader version: %s", bootloader_info_response->version);
FEAT_END

FEAT_START
    e9_printf("");
    if (executable_cmdline_request.response == NULL) {
        e9_printf("Executable command line not passed");
        break;
    }
    struct limine_executable_cmdline_response *executable_cmdline_response = executable_cmdline_request.response;
    e9_printf("Executable command line feature, revision %d", executable_cmdline_response->revision);
    e9_printf("Command line: %s", executable_cmdline_response->cmdline);
FEAT_END

FEAT_START
    e9_printf("");
    if (firmware_type_request.response == NULL) {
        e9_printf("Firmware type not passed");
        break;
    }
    struct limine_firmware_type_response *firmware_type_response = firmware_type_request.response;
    e9_printf("Firmware type feature, revision %d", firmware_type_response->revision);
    e9_printf("Firmware type: %s", firmware_type_str(firmware_type_response->firmware_type));
FEAT_END

FEAT_START
    e9_printf("");
    if (executable_address_request.response == NULL) {
        e9_printf("Executable address not passed");
        break;
    }
    struct limine_executable_address_response *exec_addr_response = executable_address_request.response;
    e9_printf("Executable address feature, revision %d", exec_addr_response->revision);
    e9_printf("Physical base: %x", exec_addr_response->physical_base);
    e9_printf("Virtual base: %x", exec_addr_response->virtual_base);
FEAT_END

FEAT_START
    e9_printf("");
    if (hhdm_request.response == NULL) {
        e9_printf("HHDM not passed");
        break;
    }
    struct limine_hhdm_response *hhdm_response = hhdm_request.response;
    e9_printf("HHDM feature, revision %d", hhdm_response->revision);
    e9_printf("Higher half direct map at: %x", hhdm_response->offset);
FEAT_END

FEAT_START
    e9_printf("");
    if (memmap_request.response == NULL) {
        e9_printf("Memory map not passed");
        break;
    }
    struct limine_memmap_response *memmap_response = memmap_request.response;
    e9_printf("Memory map feature, revision %d", memmap_response->revision);
    e9_printf("%d memory map entries", memmap_response->entry_count);
    for (size_t i = 0; i < memmap_response->entry_count; i++) {
        struct limine_memmap_entry *e = memmap_response->entries[i];
        e9_printf("%x->%x %s", e->base, e->base + e->length, get_memmap_type(e->type));
    }
FEAT_END

FEAT_START
    e9_printf("");
    if (framebuffer_request.response == NULL) {
        e9_printf("Framebuffer not passed");
        break;
    }
    struct limine_framebuffer_response *fb_response = framebuffer_request.response;
    e9_printf("Framebuffers feature, revision %d", fb_response->revision);
    e9_printf("%d framebuffer(s)", fb_response->framebuffer_count);
    for (size_t i = 0; i < fb_response->framebuffer_count; i++) {
        struct limine_framebuffer *fb = fb_response->framebuffers[i];
        e9_printf("Address: %x", fb->address);
        e9_printf("Width: %d", fb->width);
        e9_printf("Height: %d", fb->height);
        e9_printf("Pitch: %d", fb->pitch);
        e9_printf("BPP: %d", fb->bpp);
        e9_printf("Memory model: %d", fb->memory_model);
        e9_printf("Red mask size: %d", fb->red_mask_size);
        e9_printf("Red mask shift: %d", fb->red_mask_shift);
        e9_printf("Green mask size: %d", fb->green_mask_size);
        e9_printf("Green mask shift: %d", fb->green_mask_shift);
        e9_printf("Blue mask size: %d", fb->blue_mask_size);
        e9_printf("Blue mask shift: %d", fb->blue_mask_shift);
        e9_printf("EDID size: %d", fb->edid_size);
        e9_printf("EDID at: %x", fb->edid);
        e9_printf("Video modes:");
        for (size_t j = 0; j < fb->mode_count; j++) {
            e9_printf("  %dx%dx%d", fb->modes[j]->width, fb->modes[j]->height, fb->modes[j]->bpp);
        }
    }
FEAT_END

FEAT_START
    e9_printf("");
    if (exec_file_request.response == NULL) {
        e9_printf("Executable file not passed");
        break;
    }
    struct limine_executable_file_response *exec_file_response = exec_file_request.response;
    e9_printf("Executable file feature, revision %d", exec_file_response->revision);
    print_file(exec_file_response->executable_file);
FEAT_END

FEAT_START
    e9_printf("");
    if (module_request.response == NULL) {
        e9_printf("Modules not passed");
        break;
    }
    struct limine_module_response *module_response = module_request.response;
    e9_printf("Modules feature, revision %d", module_response->revision);
    e9_printf("%d module(s)", module_response->module_count);
    for (size_t i = 0; i < module_response->module_count; i++) {
        struct limine_file *f = module_response->modules[i];
        e9_printf("---");
        print_file(f);
    }
FEAT_END

FEAT_START
    e9_printf("");
    if (rsdp_request.response == NULL) {
        e9_printf("RSDP not passed");
        break;
    }
    struct limine_rsdp_response *rsdp_response = rsdp_request.response;
    e9_printf("RSDP feature, revision %d", rsdp_response->revision);
    e9_printf("RSDP at: %x", rsdp_response->address);
FEAT_END

FEAT_START
    e9_printf("");
    if (smbios_request.response == NULL) {
        e9_printf("SMBIOS not passed");
        break;
    }
    struct limine_smbios_response *smbios_response = smbios_request.response;
    e9_printf("SMBIOS feature, revision %d", smbios_response->revision);
    e9_printf("SMBIOS 32-bit entry at: %x", smbios_response->entry_32);
    e9_printf("SMBIOS 64-bit entry at: %x", smbios_response->entry_64);
FEAT_END

FEAT_START
    e9_printf("");
    if (est_request.response == NULL) {
        e9_printf("EFI system table not passed");
        break;
    }
    struct limine_efi_system_table_response *est_response = est_request.response;
    e9_printf("EFI system table feature, revision %d", est_response->revision);
    e9_printf("EFI system table at: %x", est_response->address);
FEAT_END

FEAT_START
    e9_printf("");
    if (efi_memmap_request.response == NULL) {
        e9_printf("EFI memory map not passed");
        break;
    }
    struct limine_efi_memmap_response *efi_memmap_response = efi_memmap_request.response;
    e9_printf("EFI memory map feature, revision %d", efi_memmap_response->revision);
    e9_printf("EFI memory map at: %x", efi_memmap_response->memmap);
    e9_printf("EFI memory map size: %x", efi_memmap_response->memmap_size);
    e9_printf("EFI memory descriptor size: %x", efi_memmap_response->desc_size);
    e9_printf("EFI memory descriptor version: %d", efi_memmap_response->desc_version);
FEAT_END

FEAT_START
    e9_printf("");
    if (date_at_boot_request.response == NULL) {
        e9_printf("Boot time not passed");
        break;
    }
    struct limine_date_at_boot_response *date_at_boot_response = date_at_boot_request.response;
    e9_printf("Date at boot feature, revision %d", date_at_boot_response->revision);
    e9_printf("Timestamp: %d", date_at_boot_response->timestamp);
FEAT_END

// TODO: LoongArch MP
#ifndef __loongarch__
FEAT_START
    e9_printf("");
    if (_mp_request.response == NULL) {
        e9_printf("MP info not passed");
        break;
    }
    struct limine_mp_response *mp_response = _mp_request.response;
    e9_printf("MP feature, revision %d", mp_response->revision);
    e9_printf("Flags: %x", mp_response->flags);
#if defined (__x86_64__)
    e9_printf("BSP LAPIC ID: %x", mp_response->bsp_lapic_id);
#elif defined (__aarch64__)
    e9_printf("BSP MPIDR: %x", mp_response->bsp_mpidr);
#elif defined (__riscv)
    e9_printf("BSP Hart ID: %x", mp_response->bsp_hartid);
#endif
    e9_printf("CPU count: %d", mp_response->cpu_count);
    for (size_t i = 0; i < mp_response->cpu_count; i++) {
        struct limine_mp_info *cpu = mp_response->cpus[i];
        e9_printf("Processor ID: %x", cpu->processor_id);
#if defined (__x86_64__)
        e9_printf("LAPIC ID: %x", cpu->lapic_id);
#elif defined (__aarch64__)
        e9_printf("MPIDR: %x", cpu->mpidr);
#elif defined (__riscv)
        e9_printf("Hart ID: %x", cpu->hartid);
#endif


#if defined (__x86_64__)
        if (cpu->lapic_id != mp_response->bsp_lapic_id) {
#elif defined (__aarch64__)
        if (cpu->mpidr != mp_response->bsp_mpidr) {
#elif defined (__riscv)
        if (cpu->hartid != mp_response->bsp_hartid) {
#endif
            uint32_t old_ctr = __atomic_load_n(&ctr, __ATOMIC_SEQ_CST);

            __atomic_store_n(&cpu->goto_address, ap_entry, __ATOMIC_SEQ_CST);

            while (__atomic_load_n(&ctr, __ATOMIC_SEQ_CST) == old_ctr)
                ;
        }
    }
FEAT_END
#endif

FEAT_START
    e9_printf("");
    if (_dtb_request.response == NULL) {
        e9_printf("Device tree blob not passed");
        break;
    }
    struct limine_dtb_response *dtb_response = _dtb_request.response;
    e9_printf("Device tree blob feature, revision %d", dtb_response->revision);
    e9_printf("Device tree blob pointer: %x", dtb_response->dtb_ptr);
	uint32_t dtb_magic = *(uint32_t*)dtb_response->dtb_ptr;
	e9_printf("Device tree header magic: %x", dtb_magic);
FEAT_END

FEAT_START
    e9_printf("");
    if (_pm_request.response == NULL) {
        e9_printf("Paging mode not passed");
        break;
    }
    struct limine_paging_mode_response *pm_response = _pm_request.response;
    e9_printf("Paging mode feature, revision %d", pm_response->revision);
    e9_printf("  mode: %d", pm_response->mode);
FEAT_END

#if defined (__riscv)
FEAT_START
    e9_printf("");
    struct limine_riscv_bsp_hartid_response *bsp_response = _bsp_request.response;
    if (bsp_response == NULL) {
        e9_printf("RISC-V BSP Hart ID was not passed");
        break;
    }
    e9_printf("RISC-V BSP Hart ID: %x", bsp_response->bsp_hartid);
FEAT_END
#endif

FEAT_START
    e9_printf("");
    struct limine_bootloader_performance_response *perf_response = _perf_request.response;
    if (perf_response == NULL) {
        e9_printf("Bootloader performance not passed");
        break;
    }
    e9_printf("Bootloader performance feature, revision %d", perf_response->revision);
    e9_printf("Reset time: %d usec", perf_response->reset_usec);
    e9_printf("Init time: %d usec", perf_response->init_usec);
    e9_printf("Exec time: %d usec", perf_response->exec_usec);
FEAT_END

    for (;;);
}
