#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/smbios.h>
#include <lib/acpi.h>

static size_t smbios_struct_size(struct smbios_struct_header *hdr, size_t remaining) {
    if (remaining < sizeof(struct smbios_struct_header)
     || hdr->length < sizeof(struct smbios_struct_header)
     || hdr->length > remaining) {
        return 0;
    }

    const char *string_data = (void *)((uintptr_t)hdr + hdr->length);
    size_t string_area_max = remaining - hdr->length;
    size_t i = 1;
    for (; i < string_area_max && (string_data[i - 1] != '\0' || string_data[i] != '\0'); i++) {}
    if (i >= string_area_max) {
        return 0;
    }
    return hdr->length + i + 1;
}

bool smbios_walk_start(struct smbios_walk *walk) {
    struct smbios_entry_point_32 *smbios_entry_32 = NULL;
    struct smbios_entry_point_64 *smbios_entry_64 = NULL;
    acpi_get_smbios((void **)&smbios_entry_32, (void **)&smbios_entry_64);

    void *table = NULL;
    walk->length = 0;
    walk->count = 0;

    if (smbios_entry_64 != NULL) {
        table = smbios_get_table(
            smbios_entry_64->table_address, smbios_entry_64->table_maximum_size
        );
        walk->length = smbios_entry_64->table_maximum_size;
    }
    if (table == NULL && smbios_entry_32 != NULL) {
        table = smbios_get_table(smbios_entry_32->table_address, smbios_entry_32->table_length);
        walk->count = smbios_entry_32->number_of_structures;
        walk->length = smbios_entry_32->table_length;
    }

    walk->table = (uintptr_t)table;
    walk->offset = 0;
    walk->index = 0;
    return table != NULL && walk->length != 0;
}

struct smbios_struct_header *smbios_walk_next(struct smbios_walk *walk, size_t *size) {
    if (walk->count != 0 && walk->index >= walk->count) {
        return NULL;
    }

    size_t remaining = walk->length - walk->offset;
    if (remaining < sizeof(struct smbios_struct_header)) {
        return NULL;
    }

    struct smbios_struct_header *hdr = (void *)(walk->table + walk->offset);

    // SMBIOS 3.9 section 7.49 has End-of-Table used like an Inactive
    // structure, the whole table staying indexable, so stop at it only
    // when there is no structure count to go by.
    if (hdr->type == 127 && walk->count == 0) {
        return NULL;
    }

    *size = smbios_struct_size(hdr, remaining);
    if (*size == 0) {
        return NULL;
    }

    walk->offset += *size;
    walk->index++;
    return hdr;
}
