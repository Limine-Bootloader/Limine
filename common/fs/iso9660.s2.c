#include <stdint.h>
#include <stddef.h>
#include <fs/iso9660.h>
#include <lib/misc.h>
#include <lib/libc.h>
#include <mm/pmm.h>

#define ISO9660_SECTOR_SIZE (2 << 10)

struct iso9660_context {
    struct volume *vol;
    void *root;
    uint32_t root_size;
};

struct iso9660_extent {
    uint32_t LBA;
    uint32_t size;
};

struct iso9660_file_handle {
    struct iso9660_context *context;
    uint64_t total_size;
    uint32_t extent_count;
    struct iso9660_extent *extents;
};

#define ISO9660_FLAG_DIRECTORY    0x02
#define ISO9660_FLAG_MULTI_EXTENT 0x80

#define ISO9660_FIRST_VOLUME_DESCRIPTOR 0x10
#define ISO9660_VOLUME_DESCRIPTOR_SIZE ISO9660_SECTOR_SIZE
#define ROCK_RIDGE_MAX_FILENAME 255
#define ISO9660_MAX_EXTENT_COUNT 65536

// --- Both endian structures ---
struct BE16_t { uint16_t little, big; } __attribute__((packed));
struct BE32_t { uint32_t little, big; } __attribute__((packed));

// --- Directory entries ---
struct iso9660_directory_entry {
    uint8_t length;
    uint8_t extended_attribute_length;
    struct BE32_t extent;
    struct BE32_t extent_size;
    uint8_t datetime[7];
    uint8_t flags;
    uint8_t interleaved_unit_size;
    uint8_t interleaved_gap_size;
    struct BE16_t volume_seq;
    uint8_t filename_size;
    char name[];
} __attribute__((packed));

// --- Volume descriptors ---
// VDT = Volume Descriptor Type
enum {
    ISO9660_VDT_BOOT_RECORD,
    ISO9660_VDT_PRIMARY,
    ISO9660_VDT_SUPPLEMENTARY,
    ISO9660_VDT_PARTITION_DESCRIPTOR,
    ISO9660_VDT_TERMINATOR = 255
};

struct iso9660_volume_descriptor {
    uint8_t type;
    char identifier[5];
    uint8_t version;
} __attribute__((packed));

struct iso9660_primary_volume {
    struct iso9660_volume_descriptor volume_descriptor;

    union {
        struct {
            uint8_t unused0[1];
            char system_identifier[32];
            char volume_identifier[32];
            uint8_t unused1[8];
            struct BE32_t space_size;
            uint8_t unused2[32];
            struct BE16_t set_size;
            struct BE16_t volume_seq;
            struct BE16_t LBA_size;
            struct BE32_t path_table_size;

            uint32_t LBA_path_table_little;
            uint32_t LBA_optional_path_table_little;
            uint32_t LBA_path_table_big;
            uint32_t LBA_optional_path_table_big;

            struct iso9660_directory_entry root;
        } __attribute__((packed));

        uint8_t padding[2041];
    };
} __attribute__((packed));


// --- Implementation ---
struct iso9660_contexts_node {
    struct iso9660_context context;
    struct iso9660_contexts_node *next;
};

static struct iso9660_contexts_node *contexts = NULL;

// Maximum number of volume descriptors to scan before giving up
#define ISO9660_MAX_VOLUME_DESCRIPTORS 256

// Maximum directory size to prevent memory exhaustion (64MB)
#define ISO9660_MAX_DIR_SIZE (64 * 1024 * 1024)

static void iso9660_find_PVD(struct iso9660_primary_volume *desc, struct volume *vol) {
    uint32_t lba = ISO9660_FIRST_VOLUME_DESCRIPTOR;
    uint32_t max_lba = ISO9660_FIRST_VOLUME_DESCRIPTOR + ISO9660_MAX_VOLUME_DESCRIPTORS;

    while (lba < max_lba) {
        uint64_t offset = (uint64_t)lba * ISO9660_SECTOR_SIZE;
        if (!volume_read(vol, desc, offset, sizeof(struct iso9660_primary_volume))) {
            panic(false, "ISO9660: failed to read volume descriptor");
        }

        switch (desc->volume_descriptor.type) {
        case ISO9660_VDT_PRIMARY:
            return;
        case ISO9660_VDT_TERMINATOR:
            panic(false, "ISO9660: no primary volume descriptor");
            break;
        }

        ++lba;
    }

    panic(false, "ISO9660: exceeded maximum volume descriptor search limit");
}

static void iso9660_cache_root(struct volume *vol,
                               void **root,
                               uint32_t *root_size) {
    struct iso9660_primary_volume pv;
    iso9660_find_PVD(&pv, vol);

    *root_size = pv.root.extent_size.little;

    // Validate root directory size to prevent memory exhaustion, and require
    // sector alignment so directory-traversal sector-skip arithmetic is sound.
    if (*root_size == 0 || *root_size > ISO9660_MAX_DIR_SIZE
     || *root_size % ISO9660_SECTOR_SIZE != 0) {
        panic(false, "ISO9660: Invalid root directory size");
    }

    *root = ext_mem_alloc(*root_size);
    uint64_t offset = (uint64_t)pv.root.extent.little * ISO9660_SECTOR_SIZE;
    if (!volume_read(vol, *root, offset, *root_size)) {
        panic(false, "ISO9660: failed to read root directory");
    }
}

static struct iso9660_context *iso9660_get_context(struct volume *vol) {
    struct iso9660_contexts_node *current = contexts;
    while (current) {
        if (current->context.vol == vol)
            return &current->context;
        current = current->next;
    }

    // The context is not cached at this point
    struct iso9660_contexts_node *node = ext_mem_alloc(sizeof(struct iso9660_contexts_node));
    node->context.vol = vol;
    iso9660_cache_root(vol, &node->context.root, &node->context.root_size);

    node->next = contexts;
    contexts = node;
    return &node->context;
}

static bool load_name(char *buf, size_t limit, struct iso9660_directory_entry *entry) {
    // Validate entry->length is large enough
    if (entry->length < sizeof(struct iso9660_directory_entry) + entry->filename_size) {
        goto use_iso_name;
    }

    unsigned char* sysarea = ((unsigned char*)entry) + sizeof(struct iso9660_directory_entry) + entry->filename_size;
    size_t sysarea_len = entry->length - sizeof(struct iso9660_directory_entry) - entry->filename_size;
    if ((entry->filename_size & 0x1) == 0) {
        if (sysarea_len == 0) {
            goto use_iso_name;
        }
        sysarea++;
        sysarea_len--;
    }

    // Accumulate Rock Ridge name from possibly multiple NM entries
    size_t name_len = 0;
    bool found_nm = false;
    while ((sysarea_len >= 4) && (sysarea[3] == 1)) {
        if (sysarea[2] > sysarea_len || sysarea[2] == 0) {
            break;
        }
        if (sysarea[0] == 'N' && sysarea[1] == 'M' && sysarea[2] >= 5) {
            size_t frag_len = sysarea[2] - 5;
            if (name_len + frag_len >= limit) {
                panic(false, "iso9660: Filename size exceeded");
            }
            memcpy(buf + name_len, sysarea + 5, frag_len);
            name_len += frag_len;
            found_nm = true;

            // Check CONTINUE flag (bit 0 of flags byte at offset 4)
            if (!(sysarea[4] & 1)) {
                break;
            }
        }
        sysarea_len -= sysarea[2];
        sysarea += sysarea[2];
    }

    if (found_nm) {
        buf[name_len] = 0;
        return true;
    }

use_iso_name:
    name_len = entry->filename_size;
    if (name_len >= limit) {
        panic(false, "iso9660: Filename size exceeded");
    }
    // Validate that entry->length can actually hold the filename
    // entry->length must be >= sizeof(struct) + filename_size for safe access
    if (entry->length < sizeof(struct iso9660_directory_entry) + name_len) {
        // Corrupted entry: claimed filename_size exceeds actual entry data
        // Clamp name_len to what's actually available
        if (entry->length <= sizeof(struct iso9660_directory_entry)) {
            name_len = 0;
        } else {
            name_len = entry->length - sizeof(struct iso9660_directory_entry);
        }
    }
    size_t j;
    for (j = 0; j < name_len; j++) {
        if (entry->name[j] == ';')
            break;
        if (entry->name[j] == '.' && j + 1 < name_len && entry->name[j+1] == ';')
            break;
        buf[j] = entry->name[j];
    }
    buf[j] = 0;
    return false;
}

// Advance to the next directory entry in the buffer
// Returns NULL if no more entries or invalid entry
static struct iso9660_directory_entry *iso9660_next_entry(void *current, void *buffer_end) {
    struct iso9660_directory_entry *entry = current;

    if (entry->length == 0) {
        // Skip to next sector boundary
        uintptr_t current_addr = (uintptr_t)current;
        uintptr_t next_sector = ALIGN_UP(current_addr + 1, ISO9660_SECTOR_SIZE, return NULL);
        if (next_sector >= (uintptr_t)buffer_end)
            return NULL;
        entry = (struct iso9660_directory_entry *)next_sector;
        if (entry->length == 0)
            return NULL;
        return entry;
    }

    void *next = (uint8_t *)current + entry->length;
    if (next >= buffer_end)
        return NULL;

    entry = next;

    // Handle zero-length entries (padding at sector boundaries)
    if (entry->length == 0) {
        uintptr_t next_sector = ALIGN_UP((uintptr_t)next + 1, ISO9660_SECTOR_SIZE, return NULL);
        if (next_sector >= (uintptr_t)buffer_end)
            return NULL;
        entry = (struct iso9660_directory_entry *)next_sector;
        if (entry->length == 0)
            return NULL;
    }

    // Validate minimum entry size
    if (entry->length < sizeof(struct iso9660_directory_entry))
        return NULL;

    // Validate that the entire entry (as declared by its length field) is
    // within the buffer, so callers can safely read all entry->length bytes.
    if ((size_t)entry->length > (size_t)((uint8_t *)buffer_end - (uint8_t *)entry))
        return NULL;

    return entry;
}

static struct iso9660_directory_entry *iso9660_find(void *buffer, uint32_t size, const char *filename) {
    while (size) {
        struct iso9660_directory_entry *entry = buffer;

        if (entry->length == 0) {
            if (size <= ISO9660_SECTOR_SIZE)
                return NULL;
            size_t prev_size = size;
            size = ALIGN_DOWN(size, ISO9660_SECTOR_SIZE);
            // If size didn't change (was already aligned), force move to next sector
            if (prev_size == size) {
                if (size <= ISO9660_SECTOR_SIZE)
                    return NULL;
                size -= ISO9660_SECTOR_SIZE;
                buffer += ISO9660_SECTOR_SIZE;
            } else {
                buffer += prev_size - size;
            }
            continue;
        }

        // Validate entry->length doesn't exceed remaining buffer
        if (entry->length > size) {
            return NULL;  // Corrupted directory entry
        }

        // Minimum valid directory entry size
        if (entry->length < sizeof(struct iso9660_directory_entry)) {
            return NULL;  // Corrupted directory entry
        }

        char entry_filename[256];
        bool rr = load_name(entry_filename, 256, entry);

        if (rr && !case_insensitive_fopen) {
            if (strcmp(filename, entry_filename) == 0) {
                return buffer;
            }
        } else {
            if (strcasecmp(filename, entry_filename) == 0) {
                return buffer;
            }
        }

        size -= entry->length;
        buffer += entry->length;
    }

    return NULL;
}

static uint64_t iso9660_read(struct file_handle *handle, void *buf, uint64_t loc, uint64_t count);
static void iso9660_close(struct file_handle *file);

struct file_handle *iso9660_open(struct volume *vol, const char *path) {
    char buf[6];
    const uint64_t signature = ISO9660_FIRST_VOLUME_DESCRIPTOR * ISO9660_SECTOR_SIZE + 1;
    if (!volume_read(vol, buf, signature, 5)) {
        return NULL;
    }
    buf[5] = '\0';
    if (strcmp(buf, "CD001") != 0) {
        return NULL;
    }

    struct iso9660_file_handle *ret = ext_mem_alloc(sizeof(struct iso9660_file_handle));

    ret->context = iso9660_get_context(vol);

    while (*path == '/')
        ++path;

    struct iso9660_directory_entry *current = ret->context->root;
    uint32_t current_size = ret->context->root_size;

    bool first = true;

    uint32_t next_sector = 0;
    uint32_t next_size = 0;

    char filename[ROCK_RIDGE_MAX_FILENAME];
    while (true) {
        // Skip any consecutive slashes
        while (*path == '/') {
            path++;
        }

        // Check if we've reached the end of the path (handles trailing slashes)
        if (*path == '\0') {
            // Use the current directory's extent info
            // For root, this was set from ret->context->root
            // For subdirs, it was set from the last matched entry
            if (!first) {
                pmm_free(current, current_size);
            }
            pmm_free(ret, sizeof(struct iso9660_file_handle));
            return NULL;
        }

        char *aux = filename;
        char *aux_end = filename + ROCK_RIDGE_MAX_FILENAME - 1;
        while (!(*path == '/' || *path == '\0')) {
            if (aux >= aux_end) {
                panic(false, "iso9660: Path component exceeds maximum length");
            }
            *aux++ = *path++;
        }
        *aux = '\0';

        struct iso9660_directory_entry *entry = iso9660_find(current, current_size, filename);
        if (!entry) {
            if (!first) {
                pmm_free(current, current_size);
            }
            pmm_free(ret, sizeof(struct iso9660_file_handle));
            return NULL;    // Not found :(
        }

        next_sector = entry->extent.little;
        next_size = entry->extent_size.little;

        if (*path != '\0' && !(entry->flags & ISO9660_FLAG_DIRECTORY)) {
            if (!first) {
                pmm_free(current, current_size);
            }
            pmm_free(ret, sizeof(struct iso9660_file_handle));
            return NULL;
        }

        if (*path == '\0') {
            // Found the file - collect all extents for multi-extent files
            void *buffer_end = (uint8_t *)current + current_size;

            // First pass: count extents and calculate total size
            uint32_t extent_count = 1;
            uint64_t total_size = entry->extent_size.little;
            struct iso9660_directory_entry *e = entry;

            // load_name returns false on the ISO-9660 fallback path but
            // still populates the buffer; treat an empty buffer as the only
            // "no usable name" case.
            char base_name[256];
            load_name(base_name, sizeof(base_name), entry);

            while (e->flags & ISO9660_FLAG_MULTI_EXTENT) {
                struct iso9660_directory_entry *next = iso9660_next_entry(e, buffer_end);
                if (next == NULL)
                    break;
                // Per ECMA-119, multi-extent continuation records must share
                // the file identifier of the first record. Refuse to splice
                // in unrelated entries.
                char next_name[256];
                load_name(next_name, sizeof(next_name), next);
                if (base_name[0] == '\0' || strcmp(base_name, next_name) != 0) {
                    break;
                }
                e = next;
                extent_count++;
                total_size += e->extent_size.little;

                // Sanity check to prevent runaway on corrupted directories
                if (extent_count >= ISO9660_MAX_EXTENT_COUNT) {
                    break;
                }
            }

            // Allocate extent array
            ret->extents = ext_mem_alloc_counted(extent_count, sizeof(struct iso9660_extent));
            ret->extent_count = extent_count;
            ret->total_size = total_size;

            // Second pass: populate extent array
            e = entry;
            for (uint32_t i = 0; i < extent_count; i++) {
                ret->extents[i].LBA = e->extent.little;
                ret->extents[i].size = e->extent_size.little;
                if (i + 1 < extent_count) {
                    struct iso9660_directory_entry *next = iso9660_next_entry(e, buffer_end);
                    if (next == NULL)
                        break;
                    e = next;
                }
            }

            // Free the directory buffer if we allocated one
            if (!first) {
                pmm_free(current, current_size);
            }

            goto setup_handle;
        }

        path++;  // Skip the '/' separator

        if (!first) {
            pmm_free(current, current_size);
        }

        // Validate directory size to prevent memory exhaustion, and require
        // sector alignment so directory-traversal sector-skip arithmetic is
        // sound.
        if (next_size == 0 || next_size > ISO9660_MAX_DIR_SIZE
         || next_size % ISO9660_SECTOR_SIZE != 0) {
            pmm_free(ret, sizeof(struct iso9660_file_handle));
            return NULL;
        }

        current_size = next_size;
        current = ext_mem_alloc(current_size);

        first = false;

        uint64_t dir_offset = (uint64_t)next_sector * ISO9660_SECTOR_SIZE;
        if (!volume_read(vol, current, dir_offset, current_size)) {
            pmm_free(current, current_size);
            pmm_free(ret, sizeof(struct iso9660_file_handle));
            return NULL;
        }
    }

setup_handle:;
    struct file_handle *handle = ext_mem_alloc(sizeof(struct file_handle));

    handle->fd = ret;
    handle->read = (void *)iso9660_read;
    handle->close = (void *)iso9660_close;
    handle->size = ret->total_size;
    handle->vol = vol;
#if defined (UEFI)
    handle->efi_part_handle = vol->efi_part_handle;
#endif

    return handle;
}

static uint64_t iso9660_read(struct file_handle *file, void *buf, uint64_t loc, uint64_t count) {
    uint64_t requested = count;
    struct iso9660_file_handle *f = file->fd;

    // Find which extent 'loc' falls into and read across extents as needed
    uint64_t extent_start = 0;
    for (uint32_t i = 0; i < f->extent_count && count > 0; i++) {
        uint64_t extent_size = f->extents[i].size;
        uint64_t extent_end = extent_start + extent_size;

        if (loc < extent_end) {
            // Read starts (or continues) in this extent
            uint64_t offset_in_extent = (loc > extent_start) ? (loc - extent_start) : 0;
            uint64_t bytes_available = extent_size - offset_in_extent;
            uint64_t to_read = (count < bytes_available) ? count : bytes_available;

            uint64_t disk_offset = (uint64_t)f->extents[i].LBA * ISO9660_SECTOR_SIZE + offset_in_extent;

            if (!volume_read(f->context->vol, buf, disk_offset, to_read)) {
                panic(false, "iso9660: failed to read file data");
            }

            buf = (uint8_t *)buf + to_read;
            loc += to_read;
            count -= to_read;
        }

        extent_start = extent_end;
    }

    if (count > 0) {
        panic(false, "iso9660: read beyond end of file");
    }
    return requested;
}

static void iso9660_close(struct file_handle *file) {
    struct iso9660_file_handle *f = file->fd;
    pmm_free(f->extents, f->extent_count * sizeof(struct iso9660_extent));
    pmm_free(f, sizeof(struct iso9660_file_handle));
}
