#ifndef LIB__PART_H__
#define LIB__PART_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/guid.h>
#if defined (UEFI)
#  include <efi.h>
#  include <crypt/blake2b.h>
#endif

#define NO_PARTITION  (-1)
#define INVALID_TABLE (-2)
#define END_OF_TABLE  (-3)

struct volume {
#if defined (UEFI)
    EFI_HANDLE efi_handle;

    // Block storage
    EFI_HANDLE efi_part_handle;
    EFI_BLOCK_IO *block_io;

    // PXE
    EFI_PXE_BASE_CODE_PROTOCOL *pxe_base_code;

    bool unique_sector_valid;
    uint8_t unique_sector_b2b[BLAKE2B_OUT_BYTES];
#elif defined (BIOS)
    int drive;
#endif

    size_t fastest_xfer_size;

    int index;

    bool is_optical;
    bool pxe;

    int partition;
    int sector_size;
    struct volume *backing_dev;

    int max_partition;

    int cache_status;
    uint8_t *cache;
    uint64_t cached_block;

    uint64_t first_sect;
    uint64_t sect_count;

    bool guid_valid;
    struct guid guid;
    bool part_guid_valid;
    struct guid part_guid;
    bool part_type_guid_valid;
    struct guid part_type_guid;
    bool fslabel_valid;
    char *fslabel;
};

bool is_valid_mbr(struct volume *volume);

extern struct volume **volume_index;
extern size_t volume_index_i;

bool gpt_get_guid(struct guid *guid, struct volume *volume);
uint32_t mbr_get_id(struct volume *volume);

int part_get(struct volume *part, struct volume *volume, int partition);

struct volume *volume_get_by_guid(struct guid *guid);
struct volume *volume_get_by_fslabel(char *fslabel);
struct volume *volume_get_by_coord(bool optical, int drive, int partition);
#if defined (BIOS)
struct volume *volume_get_by_bios_drive(int drive);
#endif

bool volume_read(struct volume *part, void *buffer, uint64_t loc, uint64_t count);

#define volume_iterate_parts(_VOLUME_, _BODY_) do {   \
    struct volume *_VOLUME = _VOLUME_;   \
    if (_VOLUME->pxe) { \
        do { \
            struct volume *_PART = _VOLUME; \
            _BODY_ \
        } while (0); \
    } else { \
        while (_VOLUME->backing_dev != NULL) { \
            _VOLUME = _VOLUME->backing_dev; \
        } \
 \
        int _PART_CNT = -1; \
        for (size_t _PARTNO = 0; ; _PARTNO++) { \
            if (_PART_CNT > _VOLUME->max_partition) \
                break; \
 \
            struct volume *_PART = volume_get_by_coord(_VOLUME->is_optical, \
                                                       _VOLUME->index, _PARTNO); \
            if (_PART == NULL) \
                continue; \
 \
            _PART_CNT++; \
 \
            _BODY_ \
        } \
    } \
} while (0)

#endif
