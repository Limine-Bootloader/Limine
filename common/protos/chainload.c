#include <stddef.h>
#include <stdint.h>
#include <stdnoreturn.h>
#include <protos/chainload.h>
#include <lib/part.h>
#include <lib/guid.h>
#include <lib/config.h>
#include <lib/misc.h>
#include <lib/acpi.h>
#include <drivers/disk.h>
#include <lib/term.h>
#include <lib/fb.h>
#include <lib/uri.h>
#include <lib/print.h>
#include <lib/libc.h>
#include <sys/idt.h>
#include <sys/cpu.h>
#include <lib/bli.h>
#include <lib/utf.h>
#include <drivers/vga_textmode.h>
#include <mm/pmm.h>
#if defined (UEFI)
#  include <efi.h>
#endif

#if defined (BIOS)

__attribute__((noinline, section(".realmode")))
noreturn static void spinup(uint8_t drive, void *buf, uint16_t part_entry) {
    struct idtr real_mode_idt;
    real_mode_idt.limit = 0x3ff;
    real_mode_idt.ptr   = 0;

    asm volatile (
        "cli\n\t"
        "cld\n\t"

        // Safe stack location
        "mov $0x7c00, %%esp\n\t"

        // move buffer to final location
        "mov $0x7c00, %%edi\n\t"
        "mov $512, %%ecx\n\t"
        "rep movsb\n\t"

        // The boot record's DS:SI, DS being zeroed below
        "mov %%ebx, %%esi\n\t"

        "lidt (%%eax)\n\t"

        "pushl $0x08\n\t"
        "pushl $1f\n\t"
        "lret\n\t"
        "1: .code16\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        "movl %%cr0, %%eax\n\t"
        "andb $0xfe, %%al\n\t"
        "movl %%eax, %%cr0\n\t"
        "movl $1f, %%eax\n\t"
        "pushw $0\n\t"
        "pushw %%ax\n\t"
        "lret\n\t"
        "1:\n\t"
        "xorw %%ax, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"

        "sti\n\t"

        "pushw $0\n\t"
        "pushw $0x7c00\n\t"
        "lret\n\t"

        ".code32\n\t"
        :
        : "a" (&real_mode_idt), "d" (drive), "S"(buf), "b" ((uint32_t)part_entry)
        : "memory"
    );

    __builtin_unreachable();
}

// Defined in spinup_freebsd.asm_bios_ia32.
noreturn void spinup_freebsd(uint32_t drive, uint32_t buf, uint32_t count);

// FreeBSD's freebsd-boot partition contains gptboot, a multi-sector binary with
// no MBR signature that FreeBSD's pmbr loads whole to 0x7C00. pmbr stops the
// load at 0x90000 and truncates rather than failing.
#define FREEBSD_BOOT_TYPE_GUID "83bd6b9d-7f41-11dc-be0b-001560b84f0f"
#define FREEBSD_BOOT_LOAD_ADDR 0x7c00
#define FREEBSD_BOOT_LOAD_TOP 0x90000

// MBR boot code relocates itself to 0x600 before loading a boot record, which
// leaves its partition table at 0x7BE.
#define MBR_TABLE_ADDR 0x7be

// Finds a logical partition's EBR entry by its extent and makes the entry's
// start absolute like an MBR entry's. A link may point backwards, as the one
// to a partition made in a gap before the others does, so only a repeated EBR
// marks a loop.
static bool logical_part_entry(struct volume *p, struct mbr_entry *entry) {
    struct volume *disk = p->backing_dev;
    struct volume *extended = p->extended;
    uint64_t mult = (uint64_t)p->sector_size / 512;
    uint64_t visited[256];
    uint64_t ebr_sector = 0;

    for (size_t link = 0; link < SIZEOF_ARRAY(visited); link++) {
        uint16_t signature;
        struct mbr_entry table[4];

        visited[link] = ebr_sector;

        if (!volume_read(extended, &signature, ebr_sector * 512 + 0x1fe, sizeof(signature))
         || signature != 0xaa55
         || !volume_read(extended, table, ebr_sector * 512 + 0x1be, sizeof(table))) {
            return false;
        }

        bool have_next = false;
        uint64_t next = 0;

        for (int i = 0; i < 4; i++) {
            if (table[i].sect_count == 0) {
                continue;
            }

            if (table[i].type == 0x05 || table[i].type == 0x0f || table[i].type == 0x85) {
                if (!have_next) {
                    have_next = true;
                    next = table[i].first_sect * mult;
                }
                continue;
            }

            uint64_t first_sect = extended->first_sect + ebr_sector + table[i].first_sect * mult;
            if (first_sect != p->first_sect) {
                continue;
            }

            // The volume of a partition running past the disk's end was cut
            // there, while its entry keeps the declared size.
            uint64_t sect_count = table[i].sect_count * mult;
            if (disk->sect_count != (uint64_t)-1 && sect_count > disk->sect_count - first_sect) {
                sect_count = disk->sect_count - first_sect;
            }

            if (sect_count != p->sect_count) {
                continue;
            }

            if (first_sect / mult > UINT32_MAX) {
                return false;
            }

            *entry = table[i];
            entry->first_sect = first_sect / mult;
            return true;
        }

        if (!have_next) {
            return false;
        }

        for (size_t i = 0; i <= link; i++) {
            if (visited[i] == next) {
                return false;
            }
        }

        ebr_sector = next;
    }

    return false;
}

// Returns the segment 0 offset of the entry for DS:SI to point at, 0 if none.
static uint16_t place_part_entry(struct volume *p) {
    struct mbr_entry table[4] = {0};
    int slot = 0;

    if (p->partition == 0 || p->part_guid_valid) {
        return 0;
    }

    if (p->partition <= 4) {
        if (!volume_read(p->backing_dev, table, 0x1be, sizeof(table))) {
            return 0;
        }
        slot = p->partition - 1;
    } else if (!logical_part_entry(p, &table[0])) {
        return 0;
    }

    // MBR code only loads the boot record of the active partition.
    for (int i = 0; i < 4; i++) {
        table[i].status = i == slot ? 0x80 : 0x00;
    }

    for (size_t i = 0; i < sizeof(table); i++) {
        mmoutb(MBR_TABLE_ADDR + i, ((uint8_t *)table)[i]);
    }

    return MBR_TABLE_ADDR + sizeof(struct mbr_entry) * slot;
}

noreturn void chainload(char *config, char *cmdline) {
    (void)cmdline;

    uint64_t val;

    int part; {
        char *part_config = config_get_value(config, 0, "PARTITION");
        if (part_config == NULL) {
            part = 0;
        } else {
            val = strtoui(part_config, NULL, 10);
            if (val > 256) {
                panic(true, "bios: BIOS partition number outside range 0-256");
            }
            part = val;
        }
    }
    int drive; {
        char *drive_config = config_get_value(config, 0, "DRIVE");
        if (drive_config == NULL) {
            drive = boot_volume->index;
        } else {
            val = strtoui(drive_config, NULL, 10);
            if (val < 1 || val > 256) {
                panic(true, "bios: BIOS drive number outside range 1-256");
            }
            drive = val;
        }
    }

    struct volume *p = volume_get_by_coord(false, drive, part);
    if (p == NULL && config_get_value(config, 0, "GPT_GUID") == NULL
                  && config_get_value(config, 0, "GPT_UUID") == NULL
                  && config_get_value(config, 0, "MBR_ID") == NULL) {
        panic(true, "bios: Specified drive/partition not found");
    }

    char *gpt_guid_s = config_get_value(config, 0, "GPT_GUID");
    if (gpt_guid_s == NULL) {
        gpt_guid_s = config_get_value(config, 0, "GPT_UUID");
    }
    if (gpt_guid_s != NULL) {
        struct guid guid;
        if (!string_to_guid_mixed(&guid, gpt_guid_s)) {
            panic(true, "bios: Malformed GUID");
        }

        p = volume_get_by_guid(&guid);
        if (p == NULL) {
            if (!string_to_guid_be(&guid, gpt_guid_s)) {
                panic(true, "bios: Malformed GUID");
            }

            p = volume_get_by_guid(&guid);
        }

        if (p == NULL) {
            panic(true, "bios: No matching GPT drive for GPT_GUID found");
        }

        if (p->partition != 0) {
            panic(true, "bios: GPT_GUID is that of a partition, not a drive");
        }

        p = volume_get_by_coord(false, p->index, part);

        if (p == NULL) {
            panic(true, "bios: Partition specified is not valid");
        }

        goto load;
    }

    char *mbr_id_s = config_get_value(config, 0, "MBR_ID");
    if (mbr_id_s != NULL) {
        uint32_t mbr_id = strtoui(mbr_id_s, NULL, 16);

        for (size_t i = 0; i < volume_index_i; i++) {
            p = volume_index[i];

            if (!is_valid_mbr(p)) {
                continue;
            }

            uint32_t mbr_id_1;
            if (!volume_read(p, &mbr_id_1, 0x1b8, sizeof(uint32_t))) {
                continue;
            }

            if (mbr_id_1 == mbr_id) {
                p = volume_get_by_coord(false, p->index, part);

                if (p == NULL) {
                    panic(true, "bios: Partition specified is not valid");
                }

                goto load;
            }
        }

        panic(true, "bios: No matching MBR ID found");
    }

load:
    vga_textmode_init(false);

    // A freebsd-boot partition is not a normal boot record, it holds
    // gptboot, which has no 0xAA55 signature and is loaded whole by FreeBSD's
    // pmbr. Detect it by GPT type GUID and emulate the handoff.
    struct guid freebsd_boot_guid;
    if (p->part_type_guid_valid
     && string_to_guid_mixed(&freebsd_boot_guid, FREEBSD_BOOT_TYPE_GUID)
     && memcmp(&p->part_type_guid, &freebsd_boot_guid, sizeof(struct guid)) == 0) {
        // sect_count is always in 512-byte sectors, regardless of sector_size.
        uint64_t load_size = (uint64_t)p->sect_count * 512;
        // pmbr's ceiling is fixed, so it clobbers an EBDA sitting below it.
        uint64_t load_top = MIN((uint64_t)FREEBSD_BOOT_LOAD_TOP, (uint64_t)EBDA);
        if (load_size > load_top - FREEBSD_BOOT_LOAD_ADDR) {
            load_size = load_top - FREEBSD_BOOT_LOAD_ADDR;
        }
        if (load_size == 0) {
            panic(true, "bios: freebsd-boot partition has zero size");
        }

        void *fbuf = ext_mem_alloc(load_size);
        if (!volume_read(p, fbuf, 0, load_size)) {
            panic(true, "bios: Failed to read freebsd-boot partition");
        }

        spinup_freebsd(p->drive, (uint32_t)(uintptr_t)fbuf, (uint32_t)load_size);
    }

    void *buf = ext_mem_alloc(512);

    if (!volume_read(p, buf, 0, 512)) {
        panic(true, "bios: Failed to read boot sector");
    }

    uint16_t *boot_sig = (uint16_t *)(buf + 0x1fe);

    if (*boot_sig != 0xaa55) {
        panic(true, "bios: Volume is not bootable");
    }

    uint16_t part_entry = place_part_entry(p);

    spinup(p->drive, buf, part_entry);
}

#elif defined (UEFI)

static EFI_DEVICE_PATH_PROTOCOL *build_relative_efi_file_path(struct file_handle *image) {
    // The file path stored in EFI_LOADED_IMAGE_PROTOCOL::FilePath is
    // expected to be relative to the EFI_LOADED_IMAGE_PROTOCOL::DeviceHandle.
    // For this reason the EFI_DEVICE_PATH_PROTOCOL of the efi_part_handle
    // is not used as a prefix. This likely also means that the returned
    // path cannot be given to gBS->LoadImage() directly.

    size_t original_path_chars = strlen(image->path);

    // UTF-16 never takes more code units than UTF-8 takes bytes; one more is
    // for the backslash rooting the path, as firmware roots those of the
    // images it loads.
    size_t efi_file_path_alloc_len = (original_path_chars + 2) * sizeof(EFI_CHAR16);
    EFI_CHAR16 *efi_file_path = ext_mem_alloc(efi_file_path_alloc_len);

    size_t efi_file_path_chars;
    utf8_to_utf16(efi_file_path + 1, original_path_chars + 1, image->path, original_path_chars, &efi_file_path_chars);

    efi_file_path[0] = '\\';
    bool leading_slash = true;
    size_t j = 1;
    for (size_t i = 1; i <= efi_file_path_chars; i++) {
        if (efi_file_path[i] == '/' && leading_slash) {
            continue;
        }
        leading_slash = false;
        efi_file_path[j++] = efi_file_path[i] == '/' ? '\\' : efi_file_path[i];
    }
    efi_file_path[j] = 0;


    size_t efi_file_path_len = ((j + 1) * sizeof(EFI_CHAR16));
    size_t path_item_len     = sizeof(EFI_DEVICE_PATH_PROTOCOL) + efi_file_path_len;
    size_t end_item_len      = sizeof(EFI_DEVICE_PATH_PROTOCOL);
    size_t alloc_len         = path_item_len + end_item_len;

    if (path_item_len > 0xffff) {
        panic(true, "efi: Image path too long for device path node");
    }

    EFI_DEVICE_PATH_PROTOCOL *device_path;
    EFI_STATUS status = gBS->AllocatePool(EfiLoaderData, alloc_len, (void **)&device_path);
    if (status) {
        panic(true, "efi: AllocatePool() failure (%X)", (uint64_t)status);
    }

    EFI_FILEPATH_DEVICE_PATH *path_item = (EFI_FILEPATH_DEVICE_PATH *)device_path;
    path_item->Header.Type = EFI_MEDIA_DEVICE_PATH;
    path_item->Header.SubType = EFI_MEDIA_FILEPATH_DP;
    path_item->Header.Length[0] = path_item_len;
    path_item->Header.Length[1] = path_item_len >> 8;
    memcpy(&path_item->PathName, efi_file_path, efi_file_path_len);

    EFI_DEVICE_PATH_PROTOCOL *end_item = (void *)device_path + path_item_len;
    end_item->Type = EFI_END_DEVICE_PATH_TYPE;
    end_item->SubType = EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE;
    end_item->Length[0] = end_item_len;
    end_item->Length[1] = end_item_len >> 8;

    pmm_free(efi_file_path, efi_file_path_alloc_len);
    return device_path;
}

noreturn void chainload(char *config, char *cmdline) {
    char *image_path = config_get_value(config, 0, "PATH");
    if (image_path == NULL) {
        image_path = config_get_value(config, 0, "IMAGE_PATH");
    }
    if (image_path == NULL) {
        panic(true, "efi: Image path not specified");
    }

    // The firmware's LoadImage will verify the Secure Boot signature of the
    // chainloaded EFI application, so Limine does not need to enforce its
    // own hash check here.
    bool saved_secure_boot_active = secure_boot_active;
    secure_boot_active = false;

    struct file_handle *image;
    if ((image = uri_open(image_path, MEMMAP_RESERVED, false
#if defined (__i386__)
        , NULL, NULL
#endif
    )) == NULL)
        panic(true, "efi: Failed to open image with path `%s`. Is the path correct?", image_path);

    secure_boot_active = saved_secure_boot_active;

    EFI_STATUS status;

    EFI_HANDLE efi_part_handle = image->efi_part_handle;

    void *ptr = image->fd;
    size_t image_size = image->size;

    memmap_alloc_range_in(untouched_memmap, &untouched_memmap_entries,
                          (uintptr_t)ptr, ALIGN_UP(image_size, 4096, panic(true, "chainload: Alignment overflow")),
                          MEMMAP_RESERVED, MEMMAP_USABLE, true, false, true);

    EFI_DEVICE_PATH_PROTOCOL *efi_file_path = build_relative_efi_file_path(image);

    fclose(image);
    term_notready();

    size_t req_width = 0, req_height = 0, req_bpp = 0;

    char *resolution = config_get_value(config, 0, "RESOLUTION");
    if (resolution != NULL)
        parse_resolution(&req_width, &req_height, &req_bpp, resolution);

    struct fb_info *fbinfo;
    size_t fb_count;
    fb_init(&fbinfo, &fb_count, req_width, req_height, req_bpp,
            !fb_flush_reliable(), false);

    size_t cmdline_len = strlen(cmdline);
    EFI_CHAR16 *new_cmdline = NULL;
    size_t new_cmdline_size = 0;
    if (cmdline_len > 0) {
        // UTF-16 never takes more code units than UTF-8 takes bytes.
        size_t cmdline_utf16_alloc_len = CHECKED_MUL(cmdline_len + 1, sizeof(EFI_CHAR16), panic(true, "efi: Allocation size overflow"));
        EFI_CHAR16 *cmdline_utf16 = ext_mem_alloc(cmdline_utf16_alloc_len);

        size_t cmdline_utf16_chars;
        utf8_to_utf16(cmdline_utf16, cmdline_len + 1, cmdline, cmdline_len, &cmdline_utf16_chars);

        new_cmdline_size = (cmdline_utf16_chars + 1) * sizeof(EFI_CHAR16);
        status = gBS->AllocatePool(EfiLoaderData, new_cmdline_size, (void **)&new_cmdline);
        if (status) {
            panic(true, "efi: Allocation failure");
        }
        memcpy(new_cmdline, cmdline_utf16, new_cmdline_size);

        pmm_free(cmdline_utf16, cmdline_utf16_alloc_len);
    }

    pmm_release_uefi_mem();

    EFI_MEMMAP_DEVICE_PATH memdev_path[2];

    memdev_path[0].Header.Type = EFI_HARDWARE_DEVICE_PATH;
    memdev_path[0].Header.SubType = EFI_HW_MEMMAP_DP;
    memdev_path[0].Header.Length[0] = sizeof(EFI_MEMMAP_DEVICE_PATH);
    memdev_path[0].Header.Length[1] = sizeof(EFI_MEMMAP_DEVICE_PATH) >> 8;

    memdev_path[0].MemoryType       = EfiLoaderCode;
    memdev_path[0].StartingAddress  = (uintptr_t)ptr;
    memdev_path[0].EndingAddress    = (uintptr_t)ptr + image_size;

    memdev_path[1].Header.Type = EFI_END_DEVICE_PATH_TYPE;
    memdev_path[1].Header.SubType = EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE;
    memdev_path[1].Header.Length[0] = sizeof(EFI_DEVICE_PATH_PROTOCOL);
    memdev_path[1].Header.Length[1] = sizeof(EFI_DEVICE_PATH_PROTOCOL) >> 8;

    EFI_HANDLE new_handle = 0;

    status = gBS->LoadImage(0, efi_image_handle,
                            (EFI_DEVICE_PATH_PROTOCOL *)memdev_path,
                            ptr, image_size, &new_handle);
    if (status) {
        panic(false, "efi: LoadImage failure (%X)", (uint64_t)status);
    }

    EFI_GUID loaded_img_prot_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

    EFI_LOADED_IMAGE_PROTOCOL *new_handle_loaded_image = NULL;
    status = gBS->HandleProtocol(new_handle, &loaded_img_prot_guid,
                                 (void **)&new_handle_loaded_image);
    if (status) {
        panic(false, "efi: HandleProtocol failure (%X)", (uint64_t)status);
    }

    if (efi_part_handle != 0) {
        new_handle_loaded_image->DeviceHandle = efi_part_handle;
    }

    new_handle_loaded_image->FilePath = efi_file_path;

    if (cmdline_len > 0) {
        new_handle_loaded_image->LoadOptionsSize = new_cmdline_size;
        new_handle_loaded_image->LoadOptions = new_cmdline;
    } else {
        new_handle_loaded_image->LoadOptionsSize = 0;
        new_handle_loaded_image->LoadOptions = NULL;
    }

    bli_on_boot();

    EFI_UINTN exit_data_size = 0;
    EFI_CHAR16 *exit_data = NULL;
    EFI_STATUS exit_status = gBS->StartImage(new_handle, &exit_data_size, &exit_data);

    status = gBS->Exit(efi_image_handle, exit_status, exit_data_size, exit_data);
    if (status) {
        panic(false, "efi: Exit failure (%X)", (uint64_t)status);
    }

    __builtin_unreachable();
}

#endif
