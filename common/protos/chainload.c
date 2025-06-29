#include <stddef.h>
#include <stdint.h>
#include <stdnoreturn.h>
#include <protos/chainload.h>
#include <lib/part.h>
#include <lib/config.h>
#include <lib/misc.h>
#include <drivers/disk.h>
#include <lib/term.h>
#include <lib/fb.h>
#include <lib/uri.h>
#include <lib/print.h>
#include <lib/libc.h>
#include <sys/idt.h>
#include <lib/bli.h>
#include <drivers/vga_textmode.h>
#include <mm/pmm.h>
#if defined (UEFI)
#  include <efi.h>
#endif

#if defined (BIOS)

__attribute__((noinline, section(".realmode")))
noreturn static void spinup(uint8_t drive, void *buf) {
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
        : "a" (&real_mode_idt), "d" (drive), "S"(buf)
        : "memory"
    );

    __builtin_unreachable();
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

    char *gpt_guid_s = config_get_value(config, 0, "GPT_GUID");
    if (gpt_guid_s == NULL) {
        gpt_guid_s = config_get_value(config, 0, "GPT_UUID");
    }
    if (gpt_guid_s != NULL) {
        struct guid guid;
        if (!string_to_guid_be(&guid, gpt_guid_s)) {
            panic(true, "bios: Malformed GUID");
        }

        p = volume_get_by_guid(&guid);
        if (p == NULL) {
            if (!string_to_guid_mixed(&guid, gpt_guid_s)) {
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
            volume_read(p, &mbr_id_1, 0x1b8, sizeof(uint32_t));

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

    void *buf = ext_mem_alloc(512);

    volume_read(p, buf, 0, 512);

    uint16_t *boot_sig = (uint16_t *)(buf + 0x1fe);

    if (*boot_sig != 0xaa55) {
        panic(true, "bios: Volume is not bootable");
    }

    spinup(p->drive, buf);
}

#elif defined (UEFI)

/*
static void devpath_print(EFI_DEVICE_PATH_PROTOCOL *DevicePath) {
    for (;;) {
        uint8_t type = DevicePath->Type;
        uint8_t subtype = DevicePath->SubType;
        size_t length = *(uint16_t *)DevicePath->Length;

        print("Device Path Type: %x, SubType: %x, Length: %x\n", type, subtype, length);

        if (type == 4 && subtype == 4) {
            for (size_t i = sizeof(EFI_DEVICE_PATH_PROTOCOL); i < length; i += 2) {
                print("%c", *(uint16_t *)((void *)DevicePath + i));
            }
            print("\n");
        }

        DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)((uint8_t *)DevicePath + *(uint16_t *)DevicePath->Length);

        if (type == END_DEVICE_PATH_TYPE) {
            break;
        }
    }
}
*/

static size_t get_devpath_len(EFI_DEVICE_PATH_PROTOCOL *devpath) {
    size_t len = 0;
    EFI_DEVICE_PATH_PROTOCOL *header = devpath;
    for (;;) {
        size_t this_len = *(uint16_t *)header->Length;
        len += this_len;
        if (header->Type == END_DEVICE_PATH_TYPE) {
            break;
        }
        header = (void *)header + this_len;
    }
    return len;
}

static EFI_DEVICE_PATH_PROTOCOL *devpath_append(EFI_DEVICE_PATH_PROTOCOL *devpath,
                                                EFI_DEVICE_PATH_PROTOCOL *item) {
    EFI_STATUS status;

    size_t devpath_len = get_devpath_len(devpath);
    size_t item_size = *(uint16_t *)item->Length;
    size_t new_devpath_len = devpath_len + item_size;

    EFI_DEVICE_PATH_PROTOCOL *new_devpath = NULL;
    status = gBS->AllocatePool(EfiLoaderData, new_devpath_len, (void **)&new_devpath);
    if (status) {
        panic(true, "efi: AllocatePool() failure (%x)", status);
    }

    memcpy(new_devpath, devpath, devpath_len);

    EFI_DEVICE_PATH_PROTOCOL *item_ptr = (void *)new_devpath + (devpath_len - sizeof(EFI_DEVICE_PATH_PROTOCOL));
    memcpy(item_ptr, item, item_size);

    EFI_DEVICE_PATH_PROTOCOL *end_ptr = (void *)new_devpath + (new_devpath_len - sizeof(EFI_DEVICE_PATH_PROTOCOL));
    end_ptr->Type = END_DEVICE_PATH_TYPE;
    end_ptr->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    end_ptr->Length[0] = sizeof(EFI_DEVICE_PATH);
    end_ptr->Length[1] = sizeof(EFI_DEVICE_PATH) >> 8;

    return new_devpath;
}

noreturn void chainload(char *config, char *cmdline) {
    char *image_path = config_get_value(config, 0, "PATH");
    if (image_path == NULL) {
        image_path = config_get_value(config, 0, "IMAGE_PATH");
    }
    if (image_path == NULL) {
        panic(true, "efi: Image path not specified");
    }

    struct file_handle *image;
    if ((image = uri_open(image_path)) == NULL)
        panic(true, "efi: Failed to open image with path `%s`. Is the path correct?", image_path);

    EFI_STATUS status;

    size_t path_len = strlen(image->path);

    size_t efi_file_path_len = (path_len + 1) * sizeof(CHAR16);
    CHAR16 *efi_file_path = ext_mem_alloc(efi_file_path_len);

    bool leading_slash = true;
    size_t j = 0;
    for (size_t i = 0; i < path_len; i++) {
        if (image->path[i] == '/' && leading_slash) {
            continue;
        }
        leading_slash = false;
        efi_file_path[j++] = image->path[i] == '/' ? '\\' : image->path[i];
    }
    efi_file_path[j] = 0;

    EFI_GUID device_path_protocol_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *device_path = NULL;
    status = gBS->HandleProtocol(image->efi_part_handle, &device_path_protocol_guid, (void **)&device_path);
    if (status) {
        panic(true, "efi: HandleProtocol() failure (%x)", status);
    }

    fclose(image);

    size_t devpath_item_size = sizeof(EFI_DEVICE_PATH_PROTOCOL) + ((j + 1) * sizeof(CHAR16));
    EFI_DEVICE_PATH_PROTOCOL *devpath_item = ext_mem_alloc(devpath_item_size);

    devpath_item->Type = 0x04;
    devpath_item->SubType = 0x04;
    devpath_item->Length[0] = devpath_item_size;
    devpath_item->Length[1] = devpath_item_size >> 8;

    memcpy(&devpath_item[1], efi_file_path, (j + 1) * sizeof(CHAR16));
    pmm_free(efi_file_path, efi_file_path_len);

    device_path = devpath_append(device_path, devpath_item);

    pmm_free(devpath_item, devpath_item_size);

    term_notready();

    size_t req_width = 0, req_height = 0, req_bpp = 0;

    char *resolution = config_get_value(config, 0, "RESOLUTION");
    if (resolution != NULL)
        parse_resolution(&req_width, &req_height, &req_bpp, resolution);

    struct fb_info *fbinfo;
    size_t fb_count;
    fb_init(&fbinfo, &fb_count, req_width, req_height, req_bpp);

    size_t cmdline_len = strlen(cmdline);
    CHAR16 *new_cmdline;
    status = gBS->AllocatePool(EfiLoaderData, (cmdline_len + 1) * sizeof(CHAR16), (void **)&new_cmdline);
    if (status) {
        panic(true, "efi: Allocation failure");
    }
    for (size_t i = 0; i < cmdline_len + 1; i++) {
        new_cmdline[i] = cmdline[i];
    }

    pmm_release_uefi_mem();

    EFI_HANDLE new_handle = 0;

    status = gBS->LoadImage(0, efi_image_handle,
                            device_path,
                            NULL, 0, &new_handle);
    if (status) {
        panic(false, "efi: LoadImage failure (%x)", status);
    }

    EFI_GUID loaded_img_prot_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

    EFI_LOADED_IMAGE_PROTOCOL *new_handle_loaded_image = NULL;
    status = gBS->HandleProtocol(new_handle, &loaded_img_prot_guid,
                                 (void **)&new_handle_loaded_image);
    if (status) {
        panic(false, "efi: HandleProtocol failure (%x)", status);
    }

    new_handle_loaded_image->LoadOptionsSize = cmdline_len * sizeof(CHAR16);
    new_handle_loaded_image->LoadOptions = new_cmdline;

    bli_on_boot();

    UINTN exit_data_size = 0;
    CHAR16 *exit_data = NULL;
    EFI_STATUS exit_status = gBS->StartImage(new_handle, &exit_data_size, &exit_data);

    status = gBS->Exit(efi_image_handle, exit_status, exit_data_size, exit_data);
    if (status) {
        panic(false, "efi: Exit failure (%x)", status);
    }

    __builtin_unreachable();
}

#endif
