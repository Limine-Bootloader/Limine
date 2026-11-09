#ifndef LIB__SHIM_H__
#define LIB__SHIM_H__

#if defined (UEFI)

#include <stddef.h>
#include <stdbool.h>
#include <efi.h>

// The caller owns the returned image handle on success. source_buffer is borrowed.
EFI_STATUS shim_load_image(bool boot_policy, EFI_HANDLE parent_image_handle,
    EFI_DEVICE_PATH_PROTOCOL *device_path, const void *source_buffer,
    size_t source_size, EFI_HANDLE *ret_image_handle);

#endif

#endif
