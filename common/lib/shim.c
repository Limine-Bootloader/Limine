#if defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <efi.h>
#include <lib/shim.h>
#include <lib/misc.h>

EFI_STATUS shim_load_image(bool boot_policy, EFI_HANDLE parent_image_handle,
    EFI_DEVICE_PATH_PROTOCOL *device_path, const void *source_buffer,
    size_t source_size, EFI_HANDLE *ret_image_handle) {
    if (ret_image_handle == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *ret_image_handle = NULL;

    if (source_buffer == NULL || source_size == 0
     || source_size - 1 > UINTPTR_MAX - (uintptr_t)source_buffer) {
        return EFI_INVALID_PARAMETER;
    }

    if (parent_image_handle == NULL) {
        parent_image_handle = efi_image_handle;
    }

    struct {
        EFI_MEMMAP_DEVICE_PATH memory;
        EFI_DEVICE_PATH_PROTOCOL end;
    } path = {
        .memory = {
            .Header = {
                .Type = EFI_HARDWARE_DEVICE_PATH,
                .SubType = EFI_HW_MEMMAP_DP,
                .Length = { sizeof(EFI_MEMMAP_DEVICE_PATH), sizeof(EFI_MEMMAP_DEVICE_PATH) >> 8 },
            },
            .MemoryType = EfiLoaderCode,
            .StartingAddress = (uintptr_t)source_buffer,
            .EndingAddress = (uintptr_t)source_buffer + source_size - 1,
        },
        .end = {
            .Type = EFI_END_DEVICE_PATH_TYPE,
            .SubType = EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE,
            .Length = { sizeof(EFI_DEVICE_PATH_PROTOCOL), sizeof(EFI_DEVICE_PATH_PROTOCOL) >> 8 },
        },
    };

    if (device_path == NULL) {
        device_path = (EFI_DEVICE_PATH_PROTOCOL *)&path;
    }

    // Shim's image loader hooks LoadImage. Do not override a platform dbx refusal.
    EFI_STATUS status = gBS->LoadImage(boot_policy, parent_image_handle,
        device_path, (void *)source_buffer, source_size, ret_image_handle);
    if (status != EFI_SUCCESS && *ret_image_handle != NULL) {
        gBS->UnloadImage(*ret_image_handle);
        *ret_image_handle = NULL;
    }
    return status;
}

#endif
