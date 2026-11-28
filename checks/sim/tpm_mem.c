#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <efi/efiapi.h>
#include <efi/efidef.h>
#include <efi/efierr.h>
#include <lib/tpm_mem.h>

static const struct efi_memory_descriptor reserved = {
    .type = EFI_RESERVED_MEMORY_TYPE, .physical_start = 0x3000, .number_of_pages = 1
};
static size_t probe_size = sizeof(struct efi_memory_descriptor);
static size_t allocations, growths, capacity;
static void *pool;
static efi_status probe_status = EFI_BUFFER_TOO_SMALL;
static efi_status read_status = EFI_SUCCESS;
static bool oversize, fail_allocation, fail_free, stall_growth;

static EFI_API efi_status allocate_pool(uint32_t type, size_t size, void **buffer) {
    assert(type == EFI_LOADER_DATA && pool == NULL && size >= probe_size);
    if (fail_allocation) {
        return EFI_OUT_OF_RESOURCES;
    }
    pool = malloc(size);
    assert(pool != NULL);
    *buffer = pool;
    capacity = size;
    allocations++;
    return EFI_SUCCESS;
}

static EFI_API efi_status free_pool(void *buffer) {
    assert(pool != NULL && buffer == pool);
    free(buffer);
    pool = NULL;
    return fail_free ? EFI_INVALID_PARAMETER : EFI_SUCCESS;
}

static EFI_API efi_status get_memory_map(
    size_t *size,
    struct efi_memory_descriptor *map,
    size_t *key,
    size_t *stride,
    uint32_t *version
) {
    if (map == NULL) {
        assert(*size == 0 && pool == NULL);
        *size = probe_size;
        return probe_status;
    }
    assert(map == pool && *size == capacity);
    if (growths != 0) {
        growths--;
        if (stall_growth) {
            *size = capacity;
        } else {
            *size += sizeof(struct efi_memory_descriptor);
        }
        probe_size = *size;
        return EFI_BUFFER_TOO_SMALL;
    }
    if (read_status != EFI_SUCCESS) {
        return read_status;
    }
    *map = reserved;
    *key = 1;
    *stride = sizeof(struct efi_memory_descriptor);
    *version = EFI_MEMORY_DESCRIPTOR_VERSION;
    *size = oversize ? (capacity / *stride + 1) * *stride : *stride;
    return EFI_SUCCESS;
}

static void check_firmware_map(void) {
    const struct efi_boot_services bs = {
        .allocate_pool = allocate_pool, .free_pool = free_pool, .get_memory_map = get_memory_map
    };
    struct efi_memory_descriptor snapshot = reserved;
    snapshot.number_of_pages = 2;
    size_t stride = sizeof(struct efi_memory_descriptor);

    assert(tpm_log_extent(&bs, &snapshot, stride, stride, 0) == 0 && pool == NULL);
    growths = 1;
    assert(tpm_log_extent(&bs, &snapshot, stride, stride, 0x3fff) == 1);
    assert(allocations == 2 && pool == NULL);
    assert(tpm_log_extent(NULL, &snapshot, stride, stride, 0x3fff) == 0x1001);

    read_status = EFI_DEVICE_ERROR;
    assert(tpm_log_extent(&bs, &snapshot, stride, stride, 0x3fff) == 0 && pool == NULL);
    read_status = EFI_SUCCESS;
    oversize = true;
    assert(tpm_log_extent(&bs, &snapshot, stride, stride, 0x3fff) == 0 && pool == NULL);
    oversize = false;
    growths = 32;
    assert(tpm_log_extent(&bs, &snapshot, stride, stride, 0x3fff) == 0 && pool == NULL);
    assert(growths > 0 && growths < 32);
    growths = 0;
    fail_allocation = true;
    assert(tpm_log_extent(&bs, &snapshot, stride, stride, 0x3fff) == 0 && pool == NULL);
    fail_allocation = false;
    fail_free = true;
    assert(tpm_log_extent(&bs, &snapshot, stride, stride, 0x3fff) == 0 && pool == NULL);
    fail_free = false;
    growths = 2;
    stall_growth = true;
    assert(tpm_log_extent(&bs, &snapshot, stride, stride, 0x3fff) == 0 && pool == NULL);
    assert(growths == 1);
    growths = 0;
    stall_growth = false;
    probe_status = EFI_DEVICE_ERROR;
    assert(tpm_log_extent(&bs, &snapshot, stride, stride, 0x3fff) == 0 && pool == NULL);
    probe_status = EFI_SUCCESS;
    assert(tpm_log_extent(&bs, &snapshot, stride, stride, 0x3fff) == 0 && pool == NULL);
    probe_status = EFI_BUFFER_TOO_SMALL;
    probe_size = SIZE_MAX;
    assert(tpm_log_extent(&bs, &snapshot, stride, stride, 0x3fff) == 0 && pool == NULL);
    probe_size = sizeof(struct efi_memory_descriptor);
}

static void check_adjacent(void) {
    const size_t stride = sizeof(struct efi_memory_descriptor);
    struct efi_memory_descriptor map[] = {
        { .type = EFI_LOADER_DATA, .physical_start = 0x2000, .number_of_pages = 1 },
        { .type = EFI_ACPI_MEMORY_NVS, .physical_start = 0x3000, .number_of_pages = 1 },
        { .type = EFI_RESERVED_MEMORY_TYPE, .physical_start = 0x4000, .number_of_pages = 1 }
    };
    assert(tpm_ram_extent(NULL, sizeof(map), stride, 0x2000) == 0);
    assert(tpm_ram_extent((const uint8_t *)map + 1, sizeof(map), stride, 0x2000) == 0);
    assert(tpm_ram_extent(map, sizeof(map), stride, 0) == 0);
    assert(tpm_ram_extent(map, sizeof(map), stride, 0x2fff) == 0x2001);
    struct efi_memory_descriptor swap = map[0];
    map[0] = map[2];
    map[2] = swap;
    assert(tpm_ram_extent(map, sizeof(map), stride, 0x2fff) == 0x2001);
    map[1].type = EFI_MEMORY_MAPPED_IO;
    assert(tpm_ram_extent(map, sizeof(map), stride, 0x2fff) == 1);
    map[1].type = EFI_CONVENTIONAL_MEMORY;
    assert(tpm_ram_extent(map, sizeof(map), stride, 0x2fff) == 1);
    map[1].type = EFI_LOADER_DATA;
    map[1].physical_start = 0x6000;
    assert(tpm_ram_extent(map, sizeof(map), stride, 0x2fff) == 1);
    map[1].physical_start = 0x3000;
    map[1].number_of_pages = 0;
    assert(tpm_ram_extent(map, sizeof(map), stride, 0x2fff) == 1);
    map[1].number_of_pages = UINT64_MAX;
    assert(tpm_ram_extent(map, sizeof(map), stride, 0x2fff) == 1);

    struct efi_memory_descriptor top_region = {
        .type = EFI_RESERVED_MEMORY_TYPE,
        .physical_start = (uint64_t)UINTPTR_MAX - EFI_PAGE_SIZE,
        .number_of_pages = 1
    };
    assert(tpm_ram_extent(&top_region, sizeof(struct efi_memory_descriptor), stride,
                          (uintptr_t)UINTPTR_MAX - EFI_PAGE_SIZE) == EFI_PAGE_SIZE);
    top_region.physical_start = (uint64_t)UINTPTR_MAX - EFI_PAGE_SIZE + 1;
    assert(tpm_ram_extent(&top_region, sizeof(struct efi_memory_descriptor), stride,
                          (uintptr_t)UINTPTR_MAX - EFI_PAGE_SIZE + 1)
           == (UINTPTR_MAX == UINT64_MAX ? 0 : (size_t)(EFI_PAGE_SIZE - 1)));
    top_region.physical_start = 0x2000;
    top_region.number_of_pages = (UINT64_MAX - 0x2000) / EFI_PAGE_SIZE + 1;
    assert(tpm_ram_extent(&top_region, sizeof(struct efi_memory_descriptor), stride, 0x2000) == 0);
    top_region.number_of_pages = (UINT64_MAX - 0x2000) / EFI_PAGE_SIZE;
    assert(tpm_ram_extent(&top_region, sizeof(struct efi_memory_descriptor), stride, 0x2000)
           == (UINTPTR_MAX == UINT64_MAX
               ? (size_t)(top_region.number_of_pages * EFI_PAGE_SIZE)
               : (size_t)(UINTPTR_MAX - 0x2000)));
}

int main(void) {
    check_adjacent();
    check_firmware_map();
    const struct {
        struct efi_memory_descriptor region;
        uint64_t padding;
    } map[] = {
        { { .type = EFI_ACPI_MEMORY_NVS, .physical_start = 0x2000, .number_of_pages = 1 }, 0 },
        { { .type = EFI_MEMORY_MAPPED_IO, .physical_start = 0x3000, .number_of_pages = 1 }, 0 },
        { { .type = EFI_LOADER_CODE, .physical_start = 0x8000, .number_of_pages = 2 }, 0 },
        { { .type = EFI_RESERVED_MEMORY_TYPE, .physical_start = 0xa000, .number_of_pages = 1 }, 0 }
    };
    size_t stride = sizeof(map[0]);
    return tpm_ram_extent(map, sizeof(map), stride, 0x2f00) != 0x100
        || tpm_ram_extent(map, sizeof(map), stride, 0x2fff) != 1
        || tpm_ram_extent(map, sizeof(map), stride, 0x3000) != 0
        || tpm_ram_extent(map, sizeof(map), stride, 0x4100) != 0
        || tpm_ram_extent(map, sizeof(map), stride, 0x8800) != 0x2800
        || tpm_ram_extent(map, sizeof(map), stride, 0xafff) != 1
        || tpm_ram_extent(map, sizeof(map), stride, 0xb000) != 0
        || tpm_ram_extent(map, sizeof(map) - 1, stride, 0x2000) != 0
        || tpm_ram_extent(map, sizeof(map), 0, 0x2000) != 0
        || tpm_ram_extent(map, stride + 1, stride + 1, 0x2000) != 0;
}
