#include <fs/fat32.h>
#include <lib/misc.h>
#include <drivers/disk.h>
#include <lib/libc.h>
#include <lib/print.h>
#include <mm/pmm.h>
#include <stdbool.h>

#define FAT32_LFN_MAX_ENTRIES 20
#define FAT32_LFN_MAX_FILENAME_LENGTH (FAT32_LFN_MAX_ENTRIES * 13 + 1)

#define FAT32_ATTRIBUTE_SUBDIRECTORY 0x10
#define FAT32_LFN_ATTRIBUTE 0x0F
#define FAT32_ATTRIBUTE_VOLLABEL 0x08

struct fat32_context {
    struct volume *part;
    int type;
    char *label;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t number_of_fats;
    uint32_t hidden_sectors;
    uint32_t sectors_per_fat;
    uint32_t fat_start_lba;
    uint32_t data_start_lba;
    uint32_t root_directory_cluster;
    uint16_t root_entries;
    uint32_t root_start;
    uint32_t root_size;
};

struct fat32_file_handle {
    struct fat32_context context;
    uint32_t first_cluster;
    uint32_t size_bytes;
    uint32_t size_clusters;
    uint32_t *cluster_chain;
    size_t chain_len;
};

struct fat32_bpb {
    union {
        struct {
            uint8_t jump[3];
            char oem[8];
            uint16_t bytes_per_sector;
            uint8_t sectors_per_cluster;
            uint16_t reserved_sectors;
            uint8_t fats_count;
            uint16_t root_entries_count;
            uint16_t sectors_count_16;
            uint8_t media_descriptor_type;
            uint16_t sectors_per_fat_16;
            uint16_t sectors_per_track;
            uint16_t heads_count;
            uint32_t hidden_sectors_count;
            uint32_t sectors_count_32;
            uint32_t sectors_per_fat_32;
            uint16_t flags;
            uint16_t fat_version_number;
            uint32_t root_directory_cluster;
            uint16_t fs_info_sector;
            uint16_t backup_boot_sector;
            uint8_t reserved[12];
            uint8_t drive_number;
            uint8_t nt_flags;
            uint8_t signature;
            uint32_t volume_serial_number;
            char label[11];
            char system_identifier[8];
        } __attribute__((packed));
        uint8_t padding[512];
    };
} __attribute__((packed));

struct fat32_directory_entry {
    char file_name_and_ext[8 + 3];
    uint8_t attribute;
    uint8_t file_data_1[8];
    uint16_t cluster_num_high;
    uint8_t file_data_2[4];
    uint16_t cluster_num_low;
    uint32_t file_size_bytes;
} __attribute__((packed));

struct fat32_directory_handle {
    struct fat32_context context;
    struct fat32_directory_entry entry;
};

struct fat32_lfn_entry {
    uint8_t sequence_number;
    char name1[10];
    uint8_t attribute;
    uint8_t type;
    uint8_t dos_checksum;
    char name2[12];
    uint16_t first_cluster;
    char name3[4];
} __attribute__((packed));

static int fat32_open_in(struct fat32_context* context, struct fat32_directory_entry* directory, struct fat32_directory_entry* file, const char* name);

static int fat32_init_context(struct fat32_context* context, struct volume *part) {
    context->part = part;

    struct fat32_bpb bpb;
    volume_read(context->part, &bpb, 0, sizeof(struct fat32_bpb));

    // Sanity check of bpb

    // Checks for FAT12/16
    if (strncmp((((void *)&bpb) + 0x36), "FAT", 3) == 0) {
        goto signature_valid;
    }

    // Checks for FAT32
    if (strncmp((((void *)&bpb) + 0x52), "FAT", 3) == 0) {
        goto signature_valid;
    }

    // Checks for FAT32 (with 64-bit sector count)
    if (strncmp((((void *)&bpb) + 0x03), "FAT32", 5) == 0) {
        goto signature_valid;
    }

    return 1;

signature_valid:;

    const uint8_t sector_per_cluster_valid_values[] = { 1, 2, 4, 8, 16, 32, 64, 128 };
    for (size_t i = 0; i < SIZEOF_ARRAY(sector_per_cluster_valid_values); i++) {
        if (bpb.sectors_per_cluster == sector_per_cluster_valid_values[i]) {
            goto sector_per_cluster_valid;
        }
    }

    return 1;

sector_per_cluster_valid:;

    const uint16_t bytes_per_sector_valid_values[] = { 512, 1024, 2048, 4096 };
    for (size_t i = 0; i < SIZEOF_ARRAY(bytes_per_sector_valid_values); i++) {
        if (bpb.bytes_per_sector == bytes_per_sector_valid_values[i]) {
            goto bytes_per_sector_valid;
        }
    }

    return 1;

bytes_per_sector_valid:;

    // The following mess to identify the FAT type is from the FAT spec
    // at paragraph 3.5
    size_t root_dir_sects = ((bpb.root_entries_count * 32) + (bpb.bytes_per_sector - 1)) / bpb.bytes_per_sector;

    size_t data_sects = (bpb.sectors_count_16 ?: bpb.sectors_count_32) - (bpb.reserved_sectors + (bpb.fats_count * (bpb.sectors_per_fat_16 ?: bpb.sectors_per_fat_32)) + root_dir_sects);

    size_t clusters_count = data_sects / bpb.sectors_per_cluster;

    if (clusters_count < 4085) {
        context->type = 12;
    } else if (clusters_count < 65525) {
        context->type = 16;
    } else {
        context->type = 32;
    }

    context->bytes_per_sector = bpb.bytes_per_sector;
    context->sectors_per_cluster = bpb.sectors_per_cluster;
    context->reserved_sectors = bpb.reserved_sectors;
    context->number_of_fats = bpb.fats_count;
    context->hidden_sectors = bpb.hidden_sectors_count;
    context->sectors_per_fat = context->type == 32 ? bpb.sectors_per_fat_32 : bpb.sectors_per_fat_16;
    context->root_directory_cluster = bpb.root_directory_cluster;
    context->fat_start_lba = bpb.reserved_sectors;
    context->root_entries = bpb.root_entries_count;
    context->root_start = context->reserved_sectors + context->number_of_fats * context->sectors_per_fat;
    context->root_size = DIV_ROUNDUP(context->root_entries * sizeof(struct fat32_directory_entry), context->bytes_per_sector);
    switch (context->type) {
        case 12:
        case 16:
            context->data_start_lba = context->root_start + context->root_size;
            break;
        case 32:
            context->data_start_lba = context->root_start;
            break;
        default:
            __builtin_unreachable();
    }

    // get the volume label
    struct fat32_directory_entry _current_directory;
    struct fat32_directory_entry *current_directory;

    switch (context->type) {
        case 12:
        case 16:
            current_directory = NULL;
            break;
        case 32:
            _current_directory.cluster_num_low = context->root_directory_cluster & 0xFFFF;
            _current_directory.cluster_num_high = context->root_directory_cluster >> 16;
            current_directory = &_current_directory;
            break;
        default:
            __builtin_unreachable();
    }

    char *vol_label;
    if (fat32_open_in(context, current_directory, (struct fat32_directory_entry *)&vol_label, NULL) == 0) {
        context->label = vol_label;
    } else {
        context->label = NULL;
    }

    return 0;
}

static int read_cluster_from_map(struct fat32_context *context, uint32_t cluster, uint32_t *out) {
    switch (context->type) {
        case 12: {
            *out = 0;
            uint16_t tmp = 0;
            volume_read(context->part, &tmp, context->fat_start_lba * context->bytes_per_sector + (cluster + cluster / 2), sizeof(uint16_t));
            if (cluster % 2 == 0) {
                *out = tmp & 0xfff;
            } else {
                *out = tmp >> 4;
            }
            break;
        }
        case 16:
            *out = 0;
            volume_read(context->part, out, context->fat_start_lba * context->bytes_per_sector + cluster * sizeof(uint16_t), sizeof(uint16_t));
            break;
        case 32:
            volume_read(context->part, out, context->fat_start_lba * context->bytes_per_sector + cluster * sizeof(uint32_t), sizeof(uint32_t));
            *out &= 0x0fffffff;
            break;
        default:
            __builtin_unreachable();
    }

    return 0;
}

static uint32_t *cache_cluster_chain(struct fat32_context *context,
                                     uint32_t initial_cluster,
                                     size_t *_chain_length) {
    uint32_t cluster_limit = (context->type == 12 ? 0xfef     : 0)
                           | (context->type == 16 ? 0xffef    : 0)
                           | (context->type == 32 ? 0xfffffef : 0);
    if (initial_cluster < 0x2 || initial_cluster > cluster_limit)
        return NULL;
    uint32_t cluster = initial_cluster;
    size_t chain_length;
    for (chain_length = 1; ; chain_length++) {
        read_cluster_from_map(context, cluster, &cluster);
        if (cluster < 0x2 || cluster > cluster_limit)
            break;
    }
    uint32_t *cluster_chain = ext_mem_alloc(chain_length * sizeof(uint32_t));
    cluster = initial_cluster;
    for (size_t i = 0; i < chain_length; i++) {
        cluster_chain[i] = cluster;
        read_cluster_from_map(context, cluster, &cluster);
    }
    *_chain_length = chain_length;
    return cluster_chain;
}

static bool read_cluster_chain(struct fat32_context *context,
                               uint32_t *cluster_chain,
                               void *buf, uint64_t loc, uint64_t count) {
    size_t block_size = context->sectors_per_cluster * context->bytes_per_sector;
    for (uint64_t progress = 0; progress < count;) {
        uint64_t block = (loc + progress) / block_size;

        uint64_t chunk = count - progress;
        uint64_t offset = (loc + progress) % block_size;
        if (chunk > block_size - offset)
            chunk = block_size - offset;

        uint64_t base = ((uint64_t)context->data_start_lba + (cluster_chain[block] - 2) * context->sectors_per_cluster) * context->bytes_per_sector;
        volume_read(context->part, buf + progress, base + offset, chunk);

        progress += chunk;
    }

    return true;
}

// Copy ucs-2 characters to char*
static void fat32_lfncpy(char* destination, const void* source, unsigned int size) {
    for (unsigned int i = 0; i < size; i++) {
        // ignore high bytes
        *(((uint8_t*) destination) + i) = *(((uint8_t*) source) + (i * 2));
    }
}

static bool fat32_filename_to_8_3(char *dest, const char *src) {
    int i = 0, j = 0;
    bool ext = false;

    for (size_t k = 0; k < 8+3; k++)
        dest[k] = ' ';

    while (src[i]) {
        if (src[i] == '.') {
            if (ext) {
                // This is a double extension here, just give up.
                return false;
            }
            ext = true;
            j = 8;
            i++;
            continue;
        }
        if (j >= 8+3 || (j >= 8 && !ext)) {
            // Filename too long, give up.
            return false;
        }
        dest[j++] = toupper(src[i++]);
    }

    return true;
}

static int fat32_open_in(struct fat32_context* context, struct fat32_directory_entry* directory, struct fat32_directory_entry* file, const char* name) {
    size_t block_size = context->sectors_per_cluster * context->bytes_per_sector;
    char current_lfn[FAT32_LFN_MAX_FILENAME_LENGTH] = {0};

    size_t dir_chain_len;
    struct fat32_directory_entry *directory_entries;

    if (directory != NULL) {
        uint32_t current_cluster_number = directory->cluster_num_low;
        if (context->type == 32)
            current_cluster_number |= (uint32_t)directory->cluster_num_high << 16;

        uint32_t *directory_cluster_chain = cache_cluster_chain(context, current_cluster_number, &dir_chain_len);

        if (directory_cluster_chain == NULL)
            return -1;

        directory_entries = ext_mem_alloc(dir_chain_len * block_size);

        read_cluster_chain(context, directory_cluster_chain, directory_entries, 0, dir_chain_len * block_size);

        pmm_free(directory_cluster_chain, dir_chain_len * sizeof(uint32_t));
    } else {
        dir_chain_len = DIV_ROUNDUP(context->root_entries * sizeof(struct fat32_directory_entry), block_size);

        directory_entries = ext_mem_alloc(dir_chain_len * block_size);

        volume_read(context->part, directory_entries, context->root_start * context->bytes_per_sector, context->root_entries * sizeof(struct fat32_directory_entry));
    }

    int ret;

    for (size_t i = 0; i < (dir_chain_len * block_size) / sizeof(struct fat32_directory_entry); i++) {
        if (directory_entries[i].file_name_and_ext[0] == 0x00) {
            // no more entries here
            break;
        }

        if (name == NULL) {
            if (directory_entries[i].attribute != FAT32_ATTRIBUTE_VOLLABEL) {
                continue;
            }
            char *r = ext_mem_alloc(12);
            memcpy(r, directory_entries[i].file_name_and_ext, 11);
            // remove trailing spaces
            for (int j = 10; j >= 0; j--) {
                if (r[j] == ' ') {
                    r[j] = 0;
                    continue;
                }
                break;
            }
            *((char **)file) = r;
            ret = 0;
            goto out;
        }

        if (directory_entries[i].attribute == FAT32_LFN_ATTRIBUTE) {
            struct fat32_lfn_entry* lfn = (struct fat32_lfn_entry*) &directory_entries[i];

            if (lfn->sequence_number & 0b01000000) {
                // this lfn is the first entry in the table, clear the lfn buffer
                memset(current_lfn, ' ', sizeof(current_lfn));
            }

            const unsigned int lfn_index = ((lfn->sequence_number & 0b00011111) - 1U) * 13U;
            if (lfn_index >= FAT32_LFN_MAX_ENTRIES * 13) {
                continue;
            }

            fat32_lfncpy(current_lfn + lfn_index + 00, lfn->name1, 5);
            fat32_lfncpy(current_lfn + lfn_index + 05, lfn->name2, 6);
            fat32_lfncpy(current_lfn + lfn_index + 11, lfn->name3, 2);

            if (lfn_index != 0)
                continue;

            // remove trailing spaces
            for (int j = SIZEOF_ARRAY(current_lfn) - 2; j >= -1; j--) {
                if (j == -1 || current_lfn[j] != ' ') {
                    current_lfn[j + 1] = 0;
                    break;
                }
            }

            int (*strcmpfn)(const char *, const char *) = case_insensitive_fopen ? strcasecmp : strcmp;

            if (strcmpfn(current_lfn, name) == 0) {
                *file = directory_entries[i+1];
                ret = 0;
                goto out;
            }
        }

        if (directory_entries[i].attribute & (1 << 3)) {
            // It is a volume label, skip
            continue;
        }
        // SFN
        char fn[8+3];
        if (!fat32_filename_to_8_3(fn, name)) {
            continue;
        }
        if (!strncmp(directory_entries[i].file_name_and_ext, fn, 8+3)) {
            *file = directory_entries[i];
            ret = 0;
            goto out;
        }
    }

    // file not found
    ret = -1;

out:
    pmm_free(directory_entries, dir_chain_len * block_size);
    return ret;
}

char *fat32_get_label(struct volume *part) {
    struct fat32_context context;
    if (fat32_init_context(&context, part) != 0) {
        return NULL;
    }

    return context.label;
}

static void fat32_read(struct file_handle *handle, void *buf, uint64_t loc, uint64_t count);
static void fat32_close(struct file_handle *file);
static struct dir_entry *fat32_readdir(struct file_handle *file, size_t *out_size);

struct file_handle *fat32_open(struct volume *part, const char *path) {
    struct fat32_context context;
    int r = fat32_init_context(&context, part);

    if (r) {
        return NULL;
    }

    struct fat32_directory_entry _current_directory;
    struct fat32_directory_entry *current_directory;
    struct fat32_directory_entry current_file;
    unsigned int current_index = 0;
    char current_part[FAT32_LFN_MAX_FILENAME_LENGTH];

    // skip trailing slashes
    while (path[current_index] == '/') {
        current_index++;
    }

    // walk down the directory tree
    switch (context.type) {
        case 12:
        case 16:
            current_directory = NULL;
            break;
        case 32:
            _current_directory.cluster_num_low = context.root_directory_cluster & 0xFFFF;
            _current_directory.cluster_num_high = context.root_directory_cluster >> 16;
            current_directory = &_current_directory;
            break;
        default:
            __builtin_unreachable();
    }

    for (;;) {
        bool expect_directory = false;

        for (unsigned int i = 0; i < SIZEOF_ARRAY(current_part); i++) {
            if (path[i + current_index] == 0) {
                memcpy(current_part, path + current_index, i);
                current_part[i] = 0;
                expect_directory = false;
                break;
            }

            if (path[i + current_index] == '/') {
                memcpy(current_part, path + current_index, i);
                current_part[i] = 0;
                current_index += i + 1;
                expect_directory = true;
                break;
            }
        }

        if ((r = fat32_open_in(&context, current_directory, &current_file, current_part)) != 0) {
            return NULL;
        }

        if (expect_directory) {
            _current_directory = current_file;
            current_directory = &_current_directory;

            if (path[current_index] == 0) {
                struct file_handle *handle = ext_mem_alloc(sizeof(struct file_handle));
                struct fat32_directory_handle *ret = ext_mem_alloc(sizeof(struct fat32_directory_handle));

                ret->context = context;
                ret->entry = _current_directory;

                handle->fd = (void *)ret;
                handle->readdir = (void *)fat32_readdir;
                handle->vol = part;
#if defined (UEFI)
                handle->efi_part_handle = part->efi_part_handle;
#endif
                return handle;
            }
        } else {
            struct file_handle *handle = ext_mem_alloc(sizeof(struct file_handle));
            struct fat32_file_handle *ret = ext_mem_alloc(sizeof(struct fat32_file_handle));

            ret->context = context;
            ret->first_cluster = current_file.cluster_num_low;
            if (context.type == 32)
                ret->first_cluster |= (uint64_t)current_file.cluster_num_high << 16;
            ret->size_clusters = DIV_ROUNDUP(current_file.file_size_bytes, context.bytes_per_sector);
            ret->size_bytes = current_file.file_size_bytes;
            ret->cluster_chain = cache_cluster_chain(&context, ret->first_cluster, &ret->chain_len);

            handle->fd = (void *)ret;
            handle->read = (void *)fat32_read;
            handle->close = (void *)fat32_close;
            handle->size = ret->size_bytes;
            handle->vol = part;
#if defined (UEFI)
            handle->efi_part_handle = part->efi_part_handle;
#endif

            return handle;
        }
    }
}

static void fat32_read(struct file_handle *file, void *buf, uint64_t loc, uint64_t count) {
    struct fat32_file_handle *f = file->fd;
    read_cluster_chain(&f->context, f->cluster_chain, buf, loc, count);
}

static void fat32_close(struct file_handle *file) {
    struct fat32_file_handle *f = file->fd;
    pmm_free(f->cluster_chain, f->chain_len * sizeof(uint32_t));
    pmm_free(f, sizeof(struct fat32_file_handle));
}

static struct dir_entry *fat32_readdir(struct file_handle *file, size_t *out_size) {
    if (!file)
        return NULL;

    struct fat32_directory_handle *d = file->fd;
    uint32_t current_cluster_number = d->entry.cluster_num_low;
    if (d->context.type == 32)
        current_cluster_number |= (uint32_t)d->entry.cluster_num_high << 16;

    size_t dir_chain_len = 0;
    uint32_t *directory_cluster_chain = cache_cluster_chain(&d->context, current_cluster_number, &dir_chain_len);

    if (directory_cluster_chain == NULL)
        return NULL;

    struct fat32_directory_entry *directory_entries;
    size_t block_size = d->context.sectors_per_cluster * d->context.bytes_per_sector;
    directory_entries = ext_mem_alloc(dir_chain_len * block_size);
    read_cluster_chain(&d->context, directory_cluster_chain, directory_entries, 0, dir_chain_len * block_size);
    char current_lfn[FAT32_LFN_MAX_FILENAME_LENGTH] = {0};

    const size_t num_entries = (dir_chain_len * block_size) / sizeof(struct fat32_directory_entry);
    struct dir_entry *buffer = ext_mem_alloc(num_entries * sizeof(struct dir_entry));
    size_t buffer_idx = 0;

    for (size_t i = 0; i < num_entries; i++) {
        if (directory_entries[i].file_name_and_ext[0] == 0x00) {
            // no more entries here
            break;
        }

        if (directory_entries[i].attribute == FAT32_LFN_ATTRIBUTE) {
            struct fat32_lfn_entry* lfn = (struct fat32_lfn_entry*) &directory_entries[i];

            if (lfn->sequence_number & 0b01000000) {
                // this lfn is the first entry in the table, clear the lfn buffer
                memset(current_lfn, ' ', sizeof(current_lfn));
            }

            const unsigned int lfn_index = ((lfn->sequence_number & 0b00011111) - 1U) * 13U;
            if (lfn_index >= FAT32_LFN_MAX_ENTRIES * 13) {
                continue;
            }

            fat32_lfncpy(current_lfn + lfn_index + 00, lfn->name1, 5);
            fat32_lfncpy(current_lfn + lfn_index + 05, lfn->name2, 6);
            fat32_lfncpy(current_lfn + lfn_index + 11, lfn->name3, 2);

            if (lfn_index != 0)
                continue;

            // Remove trailing spaces.
            for (int j = SIZEOF_ARRAY(current_lfn) - 2; j >= -1; j--) {
                if (j == -1 || current_lfn[j] != ' ') {
                    current_lfn[j + 1] = 0;
                    break;
                }
            }

            continue;
        }

        if (directory_entries[i].attribute & (1 << 3)) {
            // It is a volume label, skip
            continue;
        }

        if (i > 0 && directory_entries[i - 1].attribute == FAT32_LFN_ATTRIBUTE) {
            memcpy(buffer[buffer_idx].name, current_lfn, sizeof(current_lfn));
        } else {
            // SFN
            char sfn[8 + 1 + 3 + 1]; // 8 + '.' + 3 + NUL
            size_t sfn_idx = 8;
            memcpy(sfn, directory_entries[i].file_name_and_ext, 8);
            // Remove trailing spaces.
            for (int j = 7; j >= 0; j--) {
                if (sfn[j] == ' ') {
                    sfn[j] = 0;
                    sfn_idx = j;
                    continue;
                }
                break;
            }
            // Check if we have a filename extension.
            if (directory_entries[i].file_name_and_ext[8] != ' ') {
                sfn[sfn_idx++] = '.';
                memcpy(sfn + sfn_idx, directory_entries[i].file_name_and_ext + 8, 3);
                for (int j = 2; j >= 0; j--) {
                    if (sfn[sfn_idx + j] == ' ') {
                        sfn[sfn_idx + j] = 0;
                        continue;
                    }
                    break;
                }
            }
            memcpy(buffer[buffer_idx].name, sfn, sizeof(sfn));
        }

        if (directory_entries[i].attribute == FAT32_ATTRIBUTE_SUBDIRECTORY) {
            buffer[buffer_idx].type = DIR_ENTRY_TYPE_DIRECTORY;
        } else {
            buffer[buffer_idx].type = DIR_ENTRY_TYPE_FILE;
        }

        buffer_idx++;
    }

    pmm_free(directory_cluster_chain, dir_chain_len * sizeof(uint32_t));

    *out_size = buffer_idx;

    return buffer;
}
