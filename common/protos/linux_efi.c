#include <config.h>

#if defined(UEFI)

#include <efi.h>
#include <protos/linux_efi.h>
#include <lib/config.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/uri.h>
#include <mm/pmm.h>

// Linux v6.18 EFI stub, LINUX_EFI_INITRD_MEDIA_GUID.
static const EFI_GUID initrd_guid =
    { 0x5568e427, 0x68fc, 0x4f3d, { 0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68 } };

static struct {
    EFI_VENDOR_DEVICE_PATH vendor;
    EFI_DEVICE_PATH_PROTOCOL end;
} __attribute__((packed)) initrd_path = {
    .vendor = {
        .Header = {
            EFI_MEDIA_DEVICE_PATH,
            EFI_MEDIA_VENDOR_DP,
            { sizeof(EFI_VENDOR_DEVICE_PATH), 0 }
        },
        .Guid = {
            0x5568e427, 0x68fc, 0x4f3d,
            { 0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68 }
        }
    },
    .end = {
        EFI_END_DEVICE_PATH_TYPE,
        EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE,
        { sizeof(EFI_DEVICE_PATH_PROTOCOL), 0 }
    }
};

static void *initrd_data;
static size_t initrd_size;
static EFI_HANDLE initrd_handle;

static EFI_STATUS EFI_API initrd_load_file(EFI_LOAD_FILE2_PROTOCOL *this,
    EFI_DEVICE_PATH_PROTOCOL *path, EFI_BOOLEAN boot_policy, EFI_UINTN *size, void *buffer);

static EFI_LOAD_FILE2_PROTOCOL initrd_protocol = { .LoadFile = initrd_load_file };

static EFI_STATUS EFI_API initrd_load_file(EFI_LOAD_FILE2_PROTOCOL *this,
    EFI_DEVICE_PATH_PROTOCOL *path, EFI_BOOLEAN boot_policy, EFI_UINTN *size, void *buffer) {
    if (this != &initrd_protocol || path == NULL || size == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (boot_policy) {
        return EFI_UNSUPPORTED;
    }
    if (memcmp(path, &initrd_path.end, sizeof(EFI_DEVICE_PATH_PROTOCOL)) != 0) {
        return EFI_NOT_FOUND;
    }
    if (buffer == NULL || *size < initrd_size) {
        *size = initrd_size;
        return EFI_BUFFER_TOO_SMALL;
    }
    memcpy(buffer, initrd_data, initrd_size);
    *size = initrd_size;
    return EFI_SUCCESS;
}

static void check_initrd_source(void) {
    EFI_GUID load_file_guid = EFI_LOAD_FILE2_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *path = (void *)&initrd_path;
    EFI_HANDLE handle = NULL;
    EFI_STATUS status = gBS->LocateDevicePath(&load_file_guid, &path, &handle);
    if (status == EFI_SUCCESS) {
        panic(false, "linux: An initrd provider is already installed");
    }
    if (status != EFI_NOT_FOUND) {
        panic(false, "linux: Failed to check the initrd provider (%X)", (uint64_t)status);
    }
    for (EFI_UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
        if (memcmp(&gST->ConfigurationTable[i].VendorGuid,
                   &initrd_guid, sizeof(EFI_GUID)) == 0) {
            panic(false, "linux: An initrd table is already installed");
        }
    }
}

static void check_file_options(const char *cmdline) {
    // Linux v6.18 EFI file-option lookup also matches inside other arguments.
    for (const char *p = cmdline; *p != 0; p++) {
        if (strncmp(p, "initrd=", 7) == 0 || strncmp(p, "dtb=", 4) == 0) {
            panic(false, "linux: Unsupported command line option in authenticated boot");
        }
    }
}

static void check_cmdline(const char *cmdline) {
    check_file_options(cmdline);
    size_t size = CHECKED_ADD(strlen(cmdline), 1,
        panic(false, "linux: Command line size overflow"));
    char *argument = ext_mem_alloc(size);
    while (*cmdline != 0) {
        char *end = argument;
        bool quoted = false;
        while (*cmdline != 0 && (!isspace((unsigned char)*cmdline) || quoted)) {
            char c = *cmdline++;
            if (c == '"') {
                quoted = !quoted;
            } else {
                *end++ = c;
            }
        }
        *end = 0;
        check_file_options(argument);
        char *value = strchr(argument, '=');
        if (value != NULL) {
            *value = 0;
        }
        if (strcmp(argument, "initrd") == 0 || strcmp(argument, "initrdmem") == 0
         || strcmp(argument, "noinitrd") == 0 || strcmp(argument, "dtb") == 0) {
            panic(false, "linux: Unsupported command line option in authenticated boot");
        }
        while (*cmdline != 0 && isspace((unsigned char)*cmdline)) {
            cmdline++;
        }
    }
    pmm_free(argument, size);
}

void linux_efi_prepare(char *config, const char *cmdline, bool linux_protocol) {
    if (linux_protocol && (config_get_value(config, 0, "DTB_PATH") != NULL
     || config_get_value(NULL, 0, "GLOBAL_DTB") != NULL)) {
        panic(false, "linux: External device trees are unavailable in authenticated boot");
    }
    if (config_get_value(config, 0, "MODULE_CMDLINE") != NULL
     || config_get_value(config, 0, "MODULE_STRING") != NULL) {
        panic(false, "linux: Initrd command lines are unavailable in authenticated boot");
    }
    if (!linux_protocol && config_get_value(config, 0, "MODULE_PATH") != NULL) {
        panic(false, "efi: Modules require the Linux protocol");
    }
    check_cmdline(cmdline);
    check_initrd_source();

    const char *cursor = config;
    size_t count = 0;
    while (config_get_value_next(&cursor, "MODULE_PATH") != NULL) {
        count++;
    }
    struct file_handle **modules = ext_mem_alloc_counted(count, sizeof(struct file_handle *));
    size_t total = 0;
    cursor = config;
    for (size_t i = 0; i < count; i++) {
        char *path = config_get_value_next(&cursor, "MODULE_PATH");
        if (path[0] == '$') {
            panic(false, "linux: Compressed initrd URIs are unavailable in authenticated boot");
        }
        // Snapshot each input once; the copy pass never reopens it.
        modules[i] = uri_open(path, MEMMAP_BOOTLOADER_RECLAIMABLE, false
#if defined(__i386__)
            , NULL, NULL
#endif
        );
        if (modules[i] == NULL) {
            panic(false, "linux: Failed to open initrd with path `%s`", path);
        }
        EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        void *filesystem = NULL;
        if (modules[i]->pxe || modules[i]->efi_part_handle == NULL
         || gBS->HandleProtocol(modules[i]->efi_part_handle, &fs_guid, &filesystem) != EFI_SUCCESS
         || filesystem == NULL) {
            panic(false, "linux: Initrd must be on an EFI filesystem");
        }
        if (modules[i]->size > SIZE_MAX) {
            panic(false, "linux: Initrd is too large");
        }
        size_t length = (size_t)modules[i]->size;
        if (length != 0) {
            total = ALIGN_UP(total, 4, panic(false, "linux: Initrd size overflow"));
            total = CHECKED_ADD(total, length, panic(false, "linux: Initrd size overflow"));
        }
    }
    if (total != 0) {
        bool allow_high = true;
#if defined(__i386__)
        allow_high = false;
#endif
        initrd_data = ext_mem_alloc_type_aligned_mode(
            total, MEMMAP_BOOTLOADER_RECLAIMABLE, 4096, allow_high);
        initrd_size = total;
        size_t offset = 0;
        for (size_t i = 0; i < count; i++) {
            size_t length = (size_t)modules[i]->size;
            if (length != 0) {
                offset = ALIGN_UP(offset, 4, panic(false, "linux: Initrd size overflow"));
                memcpy((uint8_t *)initrd_data + offset, modules[i]->fd, length);
                offset += length;
            }
        }
        // Release PMM pages before firmware allocates space for the PE image.
        size_t reserved_size = ALIGN_UP(total, 4096,
            panic(false, "linux: Initrd size overflow"));
        memmap_alloc_range_in(untouched_memmap, &untouched_memmap_entries,
            (uintptr_t)initrd_data, reserved_size,
            MEMMAP_RESERVED, MEMMAP_USABLE, true, false, true);
    }
    for (size_t i = 0; i < count; i++) {
        fclose(modules[i]);
    }
    pmm_free(modules, count * sizeof(struct file_handle *));
}

void linux_efi_register(void) {
    check_initrd_source();
    if (initrd_size == 0) {
        return;
    }
    EFI_GUID load_file_guid = EFI_LOAD_FILE2_PROTOCOL_GUID;
    EFI_GUID device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_STATUS status = gBS->InstallMultipleProtocolInterfaces(&initrd_handle,
        &device_path_guid, &initrd_path, &load_file_guid, &initrd_protocol, NULL);
    if (status != EFI_SUCCESS) {
        panic(false, "linux: Failed to install the initrd provider (%X)", (uint64_t)status);
    }
}

void linux_efi_release(void) {
    if (initrd_handle != NULL) {
        EFI_GUID load_file_guid = EFI_LOAD_FILE2_PROTOCOL_GUID;
        EFI_GUID device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
        EFI_STATUS status = gBS->UninstallMultipleProtocolInterfaces(initrd_handle,
            &device_path_guid, &initrd_path, &load_file_guid, &initrd_protocol, NULL);
        if (status != EFI_SUCCESS) {
            panic(false, "linux: Failed to remove the initrd provider (%X)", (uint64_t)status);
        }
        initrd_handle = NULL;
    }
    if (initrd_data != NULL) {
        EFI_STATUS status = gBS->FreePages((uintptr_t)initrd_data,
            DIV_ROUNDUP(initrd_size, 4096, panic(false, "linux: Initrd size overflow")));
        if (status != EFI_SUCCESS) {
            panic(false, "linux: Failed to free the initrd (%X)", (uint64_t)status);
        }
        initrd_data = NULL;
        initrd_size = 0;
    }
}

#endif
