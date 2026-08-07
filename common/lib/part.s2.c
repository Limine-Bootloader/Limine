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

// Bitwise: this is stage 2, where a table costs more space than the loop costs
// time over the few kilobytes a GPT occupies.
static uint32_t crc32_update(uint32_t crc, const void *buffer, size_t count) {
    const uint8_t *bytes = buffer;

    for (size_t i = 0; i < count; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xedb88320 : crc >> 1;
        }
    }

    return crc;
}

// Streamed so a header claiming a whole block, or an entry array, needs no
// buffer of its own.
static bool crc32_volume_range(struct volume *volume, uint64_t loc,
                               uint64_t count, uint32_t *crc) {
    uint8_t chunk[512];

    while (count > 0) {
        uint64_t step = count < sizeof(chunk) ? count : sizeof(chunk);
        if (!volume_read(volume, chunk, loc, step)) {
            return false;
        }
        *crc = crc32_update(*crc, chunk, step);
        loc += step;
        count -= step;
    }

    return true;
}

// 64 times the 16384 bytes UEFI requires be reserved for the entry array.
#define GPT_MAX_ARRAY_SIZE (1024 * 1024)

// UEFI 2.11 section 5.3.2 requires four checks before a GPT may be used: the
// signature, the header CRC, that MyLBA names the block the header was read
// from, and the entry array CRC.
static bool gpt_verify_header(struct volume *volume,
                              struct gpt_table_header *header,
                              uint64_t header_lba, int lb_size) {
    if (strncmp(header->signature, "EFI PART", 8)) {
        return false;
    }

    if (header->revision != 0x00010000) {
        return false;
    }

    // HeaderSize bounds the CRC's extent, and is itself bounded by the defined
    // fields below and the header's own block above.
    if (header->header_size < sizeof(struct gpt_table_header)
     || (uint64_t)header->header_size > (uint64_t)lb_size) {
        return false;
    }

    if (header->my_lba != header_lba) {
        return false;
    }

    uint64_t header_loc = CHECKED_MUL(header_lba, (uint64_t)lb_size, return false);

    struct gpt_table_header zeroed = *header;
    zeroed.crc32 = 0;

    uint32_t crc = crc32_update(0xffffffff, &zeroed, sizeof(zeroed));
    if (header->header_size > sizeof(zeroed)) {
        uint64_t tail = CHECKED_ADD(header_loc, sizeof(zeroed), return false);
        if (!crc32_volume_range(volume, tail,
                                header->header_size - sizeof(zeroed), &crc)) {
            return false;
        }
    }

    if (~crc != header->crc32) {
        return false;
    }

    // "shall be set to a value of 128 x 2^n", which is to say a power of two no
    // smaller than an entry. Revisions before 2.8 allowed any multiple of 8.
    uint32_t entry_size = header->size_of_partition_entry;
    if (entry_size < sizeof(struct gpt_entry)
     || (entry_size & (entry_size - 1)) != 0) {
        return false;
    }

    uint64_t array_size = CHECKED_MUL((uint64_t)header->number_of_partition_entries,
                                      (uint64_t)entry_size, return false);
    if (array_size == 0) {
        return false;
    }

    // A resource limit, not a conformance one: the specification states no
    // maximum, and the geometry below cannot supply one because it bounds the
    // array by FirstUsableLBA, which the same table writes.
    if (array_size > GPT_MAX_ARRAY_SIZE) {
        return false;
    }

    // The array is reserved outside the usable range: it precedes FirstUsableLBA
    // on the primary, and follows LastUsableLBA and precedes its own header on
    // the alternate.
    uint64_t array_lba = header->partition_entry_lba;
    uint64_t array_blocks = (array_size + (uint64_t)lb_size - 1) / (uint64_t)lb_size;
    uint64_t array_end = CHECKED_ADD(array_lba, array_blocks, return false);

    if (header->first_usable_lba > header->last_usable_lba) {
        return false;
    }

    if (array_lba < header->first_usable_lba) {
        if (array_end > header->first_usable_lba) {
            return false;
        }
    } else if (array_lba <= header->last_usable_lba || array_end > header_lba) {
        return false;
    }

    if (volume->sect_count != (uint64_t)-1) {
        uint64_t device_blocks = volume->sect_count / (uint64_t)(lb_size / 512);
        if (header->last_usable_lba >= device_blocks || array_end > device_blocks) {
            return false;
        }
    }

    uint64_t array_loc = CHECKED_MUL(array_lba, (uint64_t)lb_size, return false);

    crc = 0xffffffff;
    if (!crc32_volume_range(volume, array_loc, array_size, &crc)) {
        return false;
    }

    return ~crc == header->partition_entry_array_crc32;
}

// Enumeration asks for one partition index at a time, so without this the entry
// array is verified once per index, and its size is set by the table being
// verified. Re-reading the header and comparing it is cheap where recomputing
// the array CRC is not.
static struct volume *gpt_memo_volume = NULL;
static struct gpt_table_header gpt_memo_header;
static uint64_t gpt_memo_lba;
static int gpt_memo_lb_size;

// A volume with no valid GPT is asked again for every partition index, and
// answering costs the whole search: two block sizes, each verifying the primary
// and the block it names. Confirmed by re-reading rather than on the pointer
// alone, so a recycled volume cannot inherit the answer.
static struct volume *gpt_memo_none_volume = NULL;
static struct gpt_table_header gpt_memo_none_block;
static bool gpt_memo_none_readable;

static bool gpt_memo_none_hit(struct volume *volume) {
    struct gpt_table_header fresh;

    if (gpt_memo_none_volume != volume) {
        return false;
    }

    if (!volume_read(volume, &fresh, 512, sizeof(fresh))) {
        return !gpt_memo_none_readable;
    }

    return gpt_memo_none_readable
        && memcmp(&fresh, &gpt_memo_none_block, sizeof(fresh)) == 0;
}

static void gpt_memo_none_store(struct volume *volume) {
    gpt_memo_none_volume = volume;
    gpt_memo_none_readable = volume_read(volume, &gpt_memo_none_block, 512,
                                         sizeof(gpt_memo_none_block));
}

static bool gpt_memo_hit(struct volume *volume,
                         struct gpt_table_header *header, int *lb_size) {
    struct gpt_table_header fresh;

    if (gpt_memo_volume != volume) {
        return false;
    }

    uint64_t loc = CHECKED_MUL(gpt_memo_lba, (uint64_t)gpt_memo_lb_size,
                               return false);

    if (!volume_read(volume, &fresh, loc, sizeof(fresh))
     || memcmp(&fresh, &gpt_memo_header, sizeof(fresh)) != 0) {
        return false;
    }

    *header = gpt_memo_header;
    *lb_size = gpt_memo_lb_size;
    return true;
}

static void gpt_memo_store(struct volume *volume,
                           const struct gpt_table_header *header,
                           uint64_t header_lba, int lb_size) {
    gpt_memo_volume = volume;
    gpt_memo_header = *header;
    gpt_memo_lba = header_lba;
    gpt_memo_lb_size = lb_size;
}

// UEFI 2.11 section 5.3.2 requires falling back to the alternate header when the
// primary does not verify, and places it in the last block. A disk imaged onto a
// larger one keeps its alternate where the smaller one ended, so the block the
// primary names is tried after the last block rather than instead of it: a
// genuine alternate at the end wins over whatever a corrupt primary points at.
static bool gpt_locate_header(struct volume *volume,
                              struct gpt_table_header *header, int *lb_size) {
    // The size a table was written for belongs to the image, not to the medium
    // it ends up on: a 512-byte-LBA GPT reads correctly from 2048-byte optical
    // media, which is what an ISOHYBRID is. So this is probed rather than taken
    // from the volume's sector size. 2048 is optical.
    int lb_guesses[] = {
        512,
        2048,
        4096
    };

    if (gpt_memo_hit(volume, header, lb_size)) {
        return true;
    }

    if (gpt_memo_none_hit(volume)) {
        return false;
    }

    for (size_t i = 0; i < SIZEOF_ARRAY(lb_guesses); i++) {
        int guess = lb_guesses[i];
        uint64_t candidates[2];
        size_t candidate_count = 0;

        if (volume_read(volume, header, (uint64_t)guess * 1, sizeof(*header))) {
            if (gpt_verify_header(volume, header, 1, guess)) {
                *lb_size = guess;
                gpt_memo_store(volume, header, 1, guess);
                return true;
            }

            // The signature is what identifies the block as a header at all,
            // so only a header that failed its CRC is followed. Without it the
            // field is not an LBA, it is whatever happens to be at offset 32.
            if (!strncmp(header->signature, "EFI PART", 8)
             && header->alternate_lba > 1) {
                candidates[candidate_count++] = header->alternate_lba;
            }
        }

        if (volume->sect_count != (uint64_t)-1 && guess >= 512) {
            uint64_t blocks = volume->sect_count / (uint64_t)(guess / 512);
            if (blocks >= 2) {
                // Ahead of whatever the primary claimed.
                if (candidate_count > 0 && candidates[0] == blocks - 1) {
                    candidate_count = 0;
                }
                candidates[candidate_count++] = blocks - 1;
                if (candidate_count == 2) {
                    uint64_t claimed = candidates[0];
                    candidates[0] = candidates[1];
                    candidates[1] = claimed;
                }
            }
        }

        for (size_t j = 0; j < candidate_count; j++) {
            uint64_t loc = CHECKED_MUL(candidates[j], (uint64_t)guess, continue);

            if (volume_read(volume, header, loc, sizeof(*header))
             && gpt_verify_header(volume, header, candidates[j], guess)) {
                *lb_size = guess;
                gpt_memo_store(volume, header, candidates[j], guess);
                return true;
            }
        }
    }

    gpt_memo_none_store(volume);

    return false;
}

bool gpt_get_guid(struct guid *guid, struct volume *volume) {
    struct gpt_table_header header = {0};
    int lb_size;

    if (!gpt_locate_header(volume, &header, &lb_size)) {
        return false;
    }

    *guid = header.disk_guid;

    return true;
}

// Maximum number of GPT partitions to bound enumeration driven by the volume's
// own entry count. Clamped rather than rejected to keep oversized tables usable.
#define MAX_GPT_PARTITIONS 256

static int gpt_get_part(struct volume *ret, struct volume *volume, int partition) {
    struct gpt_table_header header = {0};
    int lb_size;

    if (!gpt_locate_header(volume, &header, &lb_size)) {
        return INVALID_TABLE;
    }

    // parse the entries if reached here
    uint32_t entry_count = header.number_of_partition_entries;
    if (entry_count > MAX_GPT_PARTITIONS) {
        entry_count = MAX_GPT_PARTITIONS;
    }

    if ((uint32_t)partition >= entry_count)
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
    ret->part_type_guid_valid = true;
    ret->part_type_guid = entry.partition_type_guid;

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

    if (!volume_read(volume, &hint, 510, sizeof(uint16_t)))
        return false;
    if (hint != 0xaa55)
        return false;

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

// A data entry's start is relative to the EBR that carries it, where the chain
// link's is relative to the extended partition.
static bool mbr_logical_entry_valid(struct volume *extended_part, uint64_t ebr_sector,
                                    struct mbr_entry *entry, uint64_t *first_sect) {
    if (entry->type == 0 || entry->sect_count == 0) {
        return false;
    }

    uint64_t rel_first = CHECKED_ADD(ebr_sector, entry->first_sect, return false);
    if (!partition_range_valid(extended_part, rel_first, entry->sect_count)) {
        return false;
    }

    *first_sect = CHECKED_ADD(extended_part->first_sect, rel_first, return false);

    return partition_range_valid(extended_part->backing_dev, *first_sect, entry->sect_count);
}

static int mbr_get_logical_part(struct volume *ret, struct volume *extended_part,
                                int partition) {
    struct mbr_entry entry;

    // Limit partition index to prevent excessive iteration
    if (partition >= MAX_LOGICAL_PARTITIONS) {
        return END_OF_TABLE;
    }

    uint64_t ebr_sector = 0;
    uint64_t first_sect_64 = 0;
    int accepted = 0;
    bool found = false;

    // Partitions are probed in order, so carry on from where the last probe
    // stopped instead of following the chain from its head every time.
    if (extended_part->ebr_walk_index <= partition) {
        accepted = extended_part->ebr_walk_index;
        ebr_sector = extended_part->ebr_walk_sector;
    }

    for (int link = 0; link < MAX_LOGICAL_PARTITIONS; link++) {
        // The memo pairs an EBR with the count of logicals before it, so it has
        // to be taken before this EBR's own entry is counted.
        extended_part->ebr_walk_index = accepted;
        extended_part->ebr_walk_sector = ebr_sector;

        // Each EBR is an MBR-format sector of its own that is_valid_mbr() never
        // saw, and util-linux ends the chain at one lacking the signature.
        uint16_t signature;

        if (!volume_read(extended_part, &signature, ebr_sector * 512 + 510, sizeof(uint16_t))) {
            return END_OF_TABLE;
        }

        if (signature != 0xaa55) {
            return END_OF_TABLE;
        }

        uint64_t entry_offset = ebr_sector * 512 + 0x1be;

        if (!volume_read(extended_part, &entry, entry_offset, sizeof(struct mbr_entry))) {
            return END_OF_TABLE;
        }

        if (mbr_logical_entry_valid(extended_part, ebr_sector, &entry, &first_sect_64)) {
            if (accepted == partition) {
                found = true;
                break;
            }
            accepted++;
        }

        entry_offset = ebr_sector * 512 + 0x1ce;

        if (!volume_read(extended_part, &entry, entry_offset, sizeof(struct mbr_entry))) {
            return END_OF_TABLE;
        }

        if (entry.type != 0x0f && entry.type != 0x05) {
            return END_OF_TABLE;
        }

        uint64_t prev_ebr_sector = ebr_sector;
        ebr_sector = entry.first_sect;

        // Detect circular chain: if new sector points to 0 or backwards, it's invalid
        // (EBR sectors should always increase within the extended partition)
        if (ebr_sector == 0 || ebr_sector <= prev_ebr_sector) {
            return END_OF_TABLE;  // Circular or corrupted EBR chain
        }

        // Also check that ebr_sector is within the extended partition bounds
        if (ebr_sector >= extended_part->sect_count) {
            return END_OF_TABLE;  // EBR points outside extended partition
        }
    }

    if (!found) {
        return END_OF_TABLE;
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
    ret->part_type_guid_valid = false;

    return 0;
}

static int mbr_get_part(struct volume *ret, struct volume *volume, int partition) {
    if (!is_valid_mbr(volume)) {
        return INVALID_TABLE;
    }

    struct mbr_entry entry;

    if (partition > 3) {
        if (volume->ebr_part != NULL) {
            return mbr_get_logical_part(ret, volume->ebr_part, partition - 4);
        }

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

            struct volume *extended_part = ext_mem_alloc(sizeof(struct volume));

#if defined (UEFI)
            extended_part->efi_handle  = volume->efi_handle;
            extended_part->block_io    = volume->block_io;
#elif defined (BIOS)
            extended_part->drive       = volume->drive;
#endif
            extended_part->fastest_xfer_size = volume->fastest_xfer_size;
            extended_part->index       = volume->index;
            extended_part->is_optical  = volume->is_optical;
            extended_part->partition   = i + 1;
            extended_part->sector_size = volume->sector_size;
            extended_part->first_sect  = entry.first_sect;
            extended_part->sect_count  = entry.sect_count;
            extended_part->backing_dev = volume;

            volume->ebr_part = extended_part;

            return mbr_get_logical_part(ret, extended_part, partition - 4);
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
    ret->part_type_guid_valid = false;

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
