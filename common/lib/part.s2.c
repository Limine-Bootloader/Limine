#include <stddef.h>
#include <stdint.h>
#include <lib/part.h>
#include <drivers/disk.h>
#if defined (BIOS)
#  include <lib/real.h>
#endif
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <mm/pmm.h>
#include <fs/file.h>

enum {
    CACHE_NOT_READY = 0,
    CACHE_READY
};

static bool cache_block(struct volume *volume, uint64_t block) {
    if (volume->cache_status == CACHE_READY && block == volume->cached_block)
        return true;

    volume->cache_status = CACHE_NOT_READY;

    if (volume->cache == NULL)
        volume->cache =
            ext_mem_alloc(CHECKED_MUL((uint64_t)volume->fastest_xfer_size, (uint64_t)volume->sector_size,
                panic(false, "cache_block: block size overflow")));

    if (volume->first_sect % (volume->sector_size / 512)) {
        return false;
    }

    uint64_t first_sect = volume->first_sect / (volume->sector_size / 512);

    uint64_t xfer_size = volume->fastest_xfer_size;

    uint64_t block_offset = CHECKED_MUL(block, (uint64_t)volume->fastest_xfer_size, return false);
    uint64_t read_sector = CHECKED_ADD(first_sect, block_offset, return false);

    // Clamp xfer_size to remaining sectors in volume
    if (volume->sect_count != (uint64_t)-1) {
        uint64_t volume_sectors = volume->sect_count / (volume->sector_size / 512);
        uint64_t end_sector;
        if (__builtin_add_overflow(first_sect, volume_sectors, &end_sector)) {
            end_sector = UINT64_MAX;
        }
        if (read_sector >= end_sector) {
            return false;
        }
        uint64_t remaining = end_sector - read_sector;
        if (xfer_size > remaining) {
            xfer_size = remaining;
        }
    }

    int ret = disk_read_sectors(volume, volume->cache, read_sector, xfer_size);
    if (ret != DISK_SUCCESS) {
        return false;
    }

    volume->cache_status = CACHE_READY;
    volume->cached_block = block;

    return true;
}

bool volume_read(struct volume *volume, void *buffer, uint64_t loc, uint64_t count) {
    if (volume->pxe) {
        panic(false, "Attempted volume_read() on pxe");
    }

    if (volume->fastest_xfer_size == 0
     || volume->sector_size < 512 || volume->sector_size % 512 != 0) {
        return false;
    }

    if (volume->sect_count != (uint64_t)-1) {
        // sect_count is always in 512-byte sectors for both whole disks and partitions
        uint64_t part_size = CHECKED_MUL(volume->sect_count, 512, return false);
        if (loc >= part_size || count > part_size - loc) {
            return false;
        }
    }

    uint64_t block_size = volume->fastest_xfer_size * volume->sector_size;

    uint64_t progress = 0;
    while (progress < count) {
        uint64_t block = (loc + progress) / block_size;

        if (!cache_block(volume, block))
            return false;

        uint64_t chunk = count - progress;
        uint64_t offset = (loc + progress) % block_size;
        if (chunk > block_size - offset)
            chunk = block_size - offset;

        memcpy(buffer + progress, &volume->cache[offset], chunk);
        progress += chunk;
    }

    return true;
}

static bool partition_range_valid(struct volume *volume,
                                  uint64_t first_sect, uint64_t sect_count) {
    if (sect_count == 0) {
        return false;
    }

    uint64_t end_sect = CHECKED_ADD(first_sect, sect_count, return false);

    if (volume->sect_count != (uint64_t)-1 && end_sect > volume->sect_count) {
        return false;
    }

    return true;
}

struct gpt_table_header {
    // the head
    char     signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t _reserved0;

    // the partitioning info
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;

    // the guid
    struct guid disk_guid;

    // entries related
    uint64_t partition_entry_lba;
    uint32_t number_of_partition_entries;
    uint32_t size_of_partition_entry;
    uint32_t partition_entry_array_crc32;
} __attribute__((packed));

struct gpt_entry {
    struct guid partition_type_guid;

    struct guid unique_partition_guid;

    uint64_t starting_lba;
    uint64_t ending_lba;

    uint64_t attributes;

    uint16_t partition_name[36];
} __attribute__((packed));

bool gpt_get_guid(struct guid *guid, struct volume *volume) {
    struct gpt_table_header header = {0};

    int lb_guesses[] = {
        512,
        4096
    };
    int lb_size = -1;

    for (size_t i = 0; i < SIZEOF_ARRAY(lb_guesses); i++) {
        // read header, located after the first block
        if (!volume_read(volume, &header, lb_guesses[i] * 1, sizeof(header)))
            continue;

        // check the header
        // 'EFI PART'
        if (strncmp(header.signature, "EFI PART", 8))
            continue;

        lb_size = lb_guesses[i];
        break;
    }

    if (lb_size == -1) {
        return false;
    }

    if (header.revision != 0x00010000)
        return false;

    *guid = header.disk_guid;

    return true;
}

static int gpt_get_part(struct volume *ret, struct volume *volume, int partition) {
    struct gpt_table_header header = {0};

    int lb_guesses[] = {
        512,
        4096
    };
    int lb_size = -1;

    for (size_t i = 0; i < SIZEOF_ARRAY(lb_guesses); i++) {
        // read header, located after the first block
        if (!volume_read(volume, &header, lb_guesses[i] * 1, sizeof(header)))
            continue;

        // check the header
        // 'EFI PART'
        if (strncmp(header.signature, "EFI PART", 8))
            continue;

        lb_size = lb_guesses[i];
        break;
    }

    if (lb_size == -1) {
        return INVALID_TABLE;
    }

    if (header.revision != 0x00010000)
        return INVALID_TABLE;

    // parse the entries if reached here
    if ((uint32_t)partition >= header.number_of_partition_entries)
        return END_OF_TABLE;

    // Validate partition entry size (must be at least as large as our struct)
    uint32_t entry_size = header.size_of_partition_entry;
    if (entry_size < sizeof(struct gpt_entry)) {
        return INVALID_TABLE;
    }

    uint64_t entry_offset = CHECKED_MUL(header.partition_entry_lba, lb_size, return INVALID_TABLE);
    // Use actual entry size from header for offset calculation
    uint64_t partition_offset = (uint64_t)partition * entry_size;
    entry_offset = CHECKED_ADD(entry_offset, partition_offset, return INVALID_TABLE);

    struct gpt_entry entry = {0};
    if (!volume_read(volume, &entry, entry_offset, sizeof(entry))) {
        return END_OF_TABLE;
    }

    struct guid empty_guid = {0};
    if (!memcmp(&entry.unique_partition_guid, &empty_guid, sizeof(struct guid)))
        return NO_PARTITION;

    // Validate that ending_lba >= starting_lba to prevent underflow
    if (entry.ending_lba < entry.starting_lba) {
        return NO_PARTITION;  // Invalid partition geometry
    }

    // Calculate sector multiplier for lb_size conversion
    uint64_t sect_multiplier = lb_size / 512;

    uint64_t first_sect_result = CHECKED_MUL(entry.starting_lba, sect_multiplier, return NO_PARTITION);

    // Check for overflow in sect_count calculation
    // First compute partition size in logical blocks
    // Check if +1 would overflow (ending_lba == UINT64_MAX)
    uint64_t partition_size = entry.ending_lba - entry.starting_lba;
    if (partition_size == UINT64_MAX) {
        return NO_PARTITION;  // Partition size +1 would overflow
    }
    uint64_t partition_blocks = partition_size + 1;
    uint64_t sect_count_result = CHECKED_MUL(partition_blocks, sect_multiplier, return NO_PARTITION);

    if (!partition_range_valid(volume, first_sect_result, sect_count_result)) {
        return NO_PARTITION;
    }

#if defined (UEFI)
    ret->efi_handle  = volume->efi_handle;
    ret->block_io    = volume->block_io;
#elif defined (BIOS)
    ret->drive       = volume->drive;
#endif
    ret->fastest_xfer_size = volume->fastest_xfer_size;
    ret->index       = volume->index;
    ret->is_optical  = volume->is_optical;
    ret->partition   = partition + 1;
    ret->sector_size = volume->sector_size;
    ret->first_sect  = first_sect_result;
    ret->sect_count  = sect_count_result;
    ret->backing_dev = volume;

    struct guid guid;
    if (!fs_get_guid(&guid, ret)) {
        ret->guid_valid = false;
    } else {
        ret->guid_valid = true;
        ret->guid = guid;
    }

    char *fslabel = fs_get_label(ret);
    if (fslabel == NULL) {
        ret->fslabel_valid = false;
    } else {
        ret->fslabel_valid = true;
        ret->fslabel = fslabel;
    }

    ret->part_guid_valid = true;
    ret->part_guid = entry.unique_partition_guid;

    return 0;
}

struct mbr_entry {
	uint8_t status;
	uint8_t chs_first_sect[3];
	uint8_t type;
	uint8_t chs_last_sect[3];
	uint32_t first_sect;
	uint32_t sect_count;
} __attribute__((packed));

bool is_valid_mbr(struct volume *volume) {
    // Check if actually valid mbr
    uint16_t hint = 0;

    if (!volume_read(volume, &hint, 446, sizeof(uint8_t)))
        return false;
    if ((uint8_t)hint != 0x00 && (uint8_t)hint != 0x80)
        return false;
    if (!volume_read(volume, &hint, 462, sizeof(uint8_t)))
        return false;
    if ((uint8_t)hint != 0x00 && (uint8_t)hint != 0x80)
        return false;
    if (!volume_read(volume, &hint, 478, sizeof(uint8_t)))
        return false;
    if ((uint8_t)hint != 0x00 && (uint8_t)hint != 0x80)
        return false;
    if (!volume_read(volume, &hint, 494, sizeof(uint8_t)))
        return false;
    if ((uint8_t)hint != 0x00 && (uint8_t)hint != 0x80)
        return false;

    char hintc[64];
    if (!volume_read(volume, hintc, 3, 4))
        return false;
    if (memcmp(hintc, "NTFS", 4) == 0)
        return false;
    if (!volume_read(volume, hintc, 54, 3))
        return false;
    if (memcmp(hintc, "FAT", 3) == 0)
        return false;
    if (!volume_read(volume, hintc, 82, 3))
        return false;
    if (memcmp(hintc, "FAT", 3) == 0)
        return false;
    if (!volume_read(volume, hintc, 3, 5))
        return false;
    if (memcmp(hintc, "FAT32", 5) == 0)
        return false;
    if (!volume_read(volume, &hint, 1080, sizeof(uint16_t)))
        return false;
    if (hint == 0xef53)
        return false;

    return true;
}

uint32_t mbr_get_id(struct volume *volume) {
    if (!is_valid_mbr(volume)) {
        return 0;
    }

    uint32_t ret;
    if (!volume_read(volume, &ret, 0x1b8, sizeof(uint32_t))) {
        return 0;
    }

    return ret;
}

// Maximum number of logical partitions to prevent infinite loops from circular EBR chains
#define MAX_LOGICAL_PARTITIONS 256

static int mbr_get_logical_part(struct volume *ret, struct volume *extended_part,
                                int partition) {
    struct mbr_entry entry;

    // Limit partition index to prevent excessive iteration
    if (partition >= MAX_LOGICAL_PARTITIONS) {
        return END_OF_TABLE;
    }

    uint64_t ebr_sector = 0;
    uint64_t prev_ebr_sector = 0;

    for (int i = 0; i < partition; i++) {
        uint64_t entry_offset = ebr_sector * 512 + 0x1ce;

        if (!volume_read(extended_part, &entry, entry_offset, sizeof(struct mbr_entry))) {
            return END_OF_TABLE;
        }

        if (entry.type != 0x0f && entry.type != 0x05) {
            return END_OF_TABLE;
        }

        prev_ebr_sector = ebr_sector;
        ebr_sector = entry.first_sect;

        // Detect circular chain: if new sector points to 0 or backwards, it's invalid
        // (EBR sectors should always increase within the extended partition)
        if (ebr_sector == 0 || (i > 0 && ebr_sector <= prev_ebr_sector)) {
            return END_OF_TABLE;  // Circular or corrupted EBR chain
        }

        // Also check that ebr_sector is within the extended partition bounds
        if (ebr_sector >= extended_part->sect_count) {
            return END_OF_TABLE;  // EBR points outside extended partition
        }
    }

    uint64_t entry_offset = ebr_sector * 512 + 0x1be;

    if (!volume_read(extended_part, &entry, entry_offset, sizeof(struct mbr_entry))) {
        return END_OF_TABLE;
    }

    if (entry.type == 0)
        return NO_PARTITION;

    // Validate sect_count is non-zero
    if (entry.sect_count == 0) {
        return NO_PARTITION;
    }

    uint64_t logical_rel_first = CHECKED_ADD(ebr_sector, entry.first_sect, return NO_PARTITION);
    if (!partition_range_valid(extended_part, logical_rel_first, entry.sect_count)) {
        return NO_PARTITION;
    }

    uint64_t first_sect_64 = CHECKED_ADD(extended_part->first_sect, logical_rel_first, return NO_PARTITION);
    if (!partition_range_valid(extended_part->backing_dev, first_sect_64, entry.sect_count)) {
        return NO_PARTITION;
    }

#if defined (UEFI)
    ret->efi_handle  = extended_part->efi_handle;
    ret->block_io    = extended_part->block_io;
#elif defined (BIOS)
    ret->drive       = extended_part->drive;
#endif
    ret->fastest_xfer_size = extended_part->fastest_xfer_size;
    ret->index       = extended_part->index;
    ret->is_optical  = extended_part->is_optical;
    ret->partition   = partition + 4 + 1;
    ret->sector_size = extended_part->sector_size;
    ret->first_sect  = first_sect_64;
    ret->sect_count  = entry.sect_count;
    ret->backing_dev = extended_part->backing_dev;

    struct guid guid;
    if (!fs_get_guid(&guid, ret)) {
        ret->guid_valid = false;
    } else {
        ret->guid_valid = true;
        ret->guid = guid;
    }

    char *fslabel = fs_get_label(ret);
    if (fslabel == NULL) {
        ret->fslabel_valid = false;
    } else {
        ret->fslabel_valid = true;
        ret->fslabel = fslabel;
    }

    ret->part_guid_valid = false;

    return 0;
}

static int mbr_get_part(struct volume *ret, struct volume *volume, int partition) {
    if (!is_valid_mbr(volume)) {
        return INVALID_TABLE;
    }

    struct mbr_entry entry;

    if (partition > 3) {
        for (int i = 0; i < 4; i++) {
            uint64_t entry_offset = 0x1be + sizeof(struct mbr_entry) * i;

            if (!volume_read(volume, &entry, entry_offset, sizeof(struct mbr_entry))) {
                continue;
            }

            if (entry.type != 0x0f && entry.type != 0x05)
                continue;

            // Validate extended partition has non-zero size
            if (entry.sect_count == 0) {
                continue;
            }

            if (!partition_range_valid(volume, entry.first_sect, entry.sect_count)) {
                continue;
            }

            struct volume extended_part = {0};

#if defined (UEFI)
            extended_part.efi_handle  = volume->efi_handle;
            extended_part.block_io    = volume->block_io;
#elif defined (BIOS)
            extended_part.drive       = volume->drive;
#endif
            extended_part.fastest_xfer_size = volume->fastest_xfer_size;
            extended_part.index       = volume->index;
            extended_part.is_optical  = volume->is_optical;
            extended_part.partition   = i + 1;
            extended_part.sector_size = volume->sector_size;
            extended_part.first_sect  = entry.first_sect;
            extended_part.sect_count  = entry.sect_count;
            extended_part.backing_dev = volume;

            return mbr_get_logical_part(ret, &extended_part, partition - 4);
        }

        return END_OF_TABLE;
    }

    uint64_t entry_offset = 0x1be + sizeof(struct mbr_entry) * partition;

    if (!volume_read(volume, &entry, entry_offset, sizeof(struct mbr_entry))) {
        return END_OF_TABLE;
    }

    if (entry.type == 0)
        return NO_PARTITION;

    // Validate sect_count is non-zero
    if (entry.sect_count == 0) {
        return NO_PARTITION;
    }

    if (!partition_range_valid(volume, entry.first_sect, entry.sect_count)) {
        return NO_PARTITION;
    }

#if defined (UEFI)
    ret->efi_handle  = volume->efi_handle;
    ret->block_io    = volume->block_io;
#elif defined (BIOS)
    ret->drive       = volume->drive;
#endif
    ret->fastest_xfer_size = volume->fastest_xfer_size;
    ret->index       = volume->index;
    ret->is_optical  = volume->is_optical;
    ret->partition   = partition + 1;
    ret->sector_size = volume->sector_size;
    ret->first_sect  = entry.first_sect;
    ret->sect_count  = entry.sect_count;
    ret->backing_dev = volume;

    struct guid guid;
    if (!fs_get_guid(&guid, ret)) {
        ret->guid_valid = false;
    } else {
        ret->guid_valid = true;
        ret->guid = guid;
    }

    char *fslabel = fs_get_label(ret);
    if (fslabel == NULL) {
        ret->fslabel_valid = false;
    } else {
        ret->fslabel_valid = true;
        ret->fslabel = fslabel;
    }

    ret->part_guid_valid = false;

    return 0;
}

int part_get(struct volume *part, struct volume *volume, int partition) {
    int ret;

    // Validate partition index is non-negative
    if (partition < 0) {
        return NO_PARTITION;
    }

    ret = gpt_get_part(part, volume, partition);
    if (ret != INVALID_TABLE)
        return ret;

    ret = mbr_get_part(part, volume, partition);
    if (ret != INVALID_TABLE)
        return ret;

    return INVALID_TABLE;
}

struct volume **volume_index = NULL;
size_t volume_index_i = 0;

struct volume *volume_get_by_guid(struct guid *guid) {
    for (size_t i = 0; i < volume_index_i; i++) {
        if (volume_index[i]->guid_valid
         && memcmp(&volume_index[i]->guid, guid, 16) == 0) {
            return volume_index[i];
        }
        if (volume_index[i]->part_guid_valid
         && memcmp(&volume_index[i]->part_guid, guid, 16) == 0) {
            return volume_index[i];
        }
    }

    return NULL;
}

struct volume *volume_get_by_fslabel(char *fslabel) {
    for (size_t i = 0; i < volume_index_i; i++) {
        if (volume_index[i]->fslabel_valid
         && strcmp(volume_index[i]->fslabel, fslabel) == 0) {
            return volume_index[i];
        }
    }

    return NULL;
}

struct volume *volume_get_by_coord(bool optical, int drive, int partition) {
    for (size_t i = 0; i < volume_index_i; i++) {
        if (volume_index[i]->index == drive
         && volume_index[i]->is_optical == optical
         && volume_index[i]->partition == partition) {
            return volume_index[i];
        }
    }

    return NULL;
}

#if defined (BIOS)
struct volume *volume_get_by_bios_drive(int drive) {
    for (size_t i = 0; i < volume_index_i; i++) {
        if (volume_index[i]->drive == drive) {
            return volume_index[i];
        }
    }

    return NULL;
}
#endif
