#ifndef LIB__TPM_MEM_H
#define LIB__TPM_MEM_H

#include <stddef.h>
#include <stdint.h>
#include <efi/efiapi.h>
#include <efi/efidef.h>
#include <efi/efierr.h>

// GetEventLog gives the last record's start, not its end. Neither log may
// borrow its length from another buffer or reach beyond its RAM descriptor.
static inline size_t tpm_ram_extent(
    const void *map, size_t map_size, size_t stride, uintptr_t address
) {
    if (address == 0 || map == NULL || stride < sizeof(struct efi_memory_descriptor)
     || stride % _Alignof(struct efi_memory_descriptor) != 0
     || (uintptr_t)map % _Alignof(struct efi_memory_descriptor) != 0 || map_size % stride != 0) {
        return 0;
    }
    for (size_t i = 0; i < map_size / stride; i++) {
        const struct efi_memory_descriptor *entry =
            (const struct efi_memory_descriptor *)((const uint8_t *)map + i * stride);
        if (entry->physical_start > address) {
            continue;
        }
        switch (entry->type) {
            case EFI_RESERVED_MEMORY_TYPE:
            case EFI_LOADER_CODE:
            case EFI_LOADER_DATA:
            case EFI_BOOT_SERVICES_CODE:
            case EFI_BOOT_SERVICES_DATA:
            case EFI_RUNTIME_SERVICES_CODE:
            case EFI_RUNTIME_SERVICES_DATA:
            case EFI_ACPI_RECLAIM_MEMORY:
            case EFI_ACPI_MEMORY_NVS: {
                break;
            }
            default: {
                continue;
            }
        }
        if (entry->number_of_pages > (UINT64_MAX - entry->physical_start) / 4096) {
            continue;
        }
        uint64_t length = entry->number_of_pages * 4096;
        uint64_t offset = address - entry->physical_start;
        if (offset < length) {
            uint64_t left = length - offset;
            return left < UINTPTR_MAX - address ? (size_t)left : UINTPTR_MAX - address;
        }
    }
    return 0;
}

static inline size_t tpm_log_extent(
    const struct efi_boot_services *bs,
    const void *snapshot,
    size_t snapshot_size,
    size_t snapshot_stride,
    uintptr_t address
) {
    if (bs == NULL) {
        return tpm_ram_extent(snapshot, snapshot_size, snapshot_stride, address);
    }
    if (address == 0) {
        return 0;
    }

    size_t size = 0, key, stride;
    uint32_t version;
    efi_status status = bs->get_memory_map(&size, NULL, &key, &stride, &version);
    if (status != EFI_BUFFER_TOO_SMALL) {
        return 0;
    }
    for (size_t attempt = 0; attempt < 4; attempt++) {
        // AllocatePool can itself add descriptors. Only the required byte
        // count is usable from an EFI_BUFFER_TOO_SMALL response.
        if (size < sizeof(struct efi_memory_descriptor) || size > SIZE_MAX - EFI_PAGE_SIZE) {
            return 0;
        }
        size_t capacity = size + EFI_PAGE_SIZE;
        void *map = NULL;
        status = bs->allocate_pool(EFI_LOADER_DATA, capacity, &map);
        if (status != EFI_SUCCESS || map == NULL) {
            return 0;
        }

        size = capacity;
        stride = 0;
        status = bs->get_memory_map(&size, map, &key, &stride, &version);
        size_t extent = 0;
        if (status == EFI_SUCCESS && size <= capacity) {
            extent = tpm_ram_extent(map, size, stride, address);
        }
        if (bs->free_pool(map) != EFI_SUCCESS) {
            return 0;
        }
        if (status != EFI_BUFFER_TOO_SMALL || size <= capacity) {
            return extent;
        }
    }
    return 0;
}

#endif
