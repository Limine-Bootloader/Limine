#undef IS_WINDOWS
#if (defined(WIN32) || defined(_WIN32) || defined(__WIN32)) && !defined(__CYGWIN__)
#define IS_WINDOWS 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>

#ifndef LIMINE_NO_BIOS
#include "limine-bios-hdd.h"
#endif

static char *program_name = NULL;

static void perror_wrap(const char *fmt, ...) {
    int old_errno = errno;

    fprintf(stderr, "%s: ", program_name);

    va_list args;
    va_start(args, fmt);

    vfprintf(stderr, fmt, args);

    va_end(args);

    fprintf(stderr, ": %s\n", strerror(old_errno));
}

static void remove_arg(int *argc, char *argv[], int index) {
    for (int i = index; i < *argc - 1; i++) {
        argv[i] = argv[i + 1];
    }

    (*argc)--;

    argv[*argc] = NULL;
}

static inline bool mul_u64_overflow(uint64_t a, uint64_t b, uint64_t *res) {
    *res = a * b;
    return a != 0 && b > UINT64_MAX / a;
}

static inline bool add_u64_overflow(uint64_t a, uint64_t b, uint64_t *res) {
    *res = a + b;
    return a > UINT64_MAX - b;
}

#ifndef LIMINE_NO_BIOS

static bool quiet = false;

static int set_pos(FILE *stream, uint64_t pos) {
    if (sizeof(long) >= 8) {
        return fseek(stream, (long)pos, SEEK_SET);
    }

    long jump_size = (LONG_MAX / 2) + 1;
    long last_jump = pos % jump_size;
    uint64_t jumps = pos / jump_size;

    rewind(stream);

    for (uint64_t i = 0; i < jumps; i++) {
        if (fseek(stream, jump_size, SEEK_CUR) != 0) {
            return -1;
        }
    }
    if (fseek(stream, last_jump, SEEK_CUR) != 0) {
        return -1;
    }

    return 0;
}

#define SIZEOF_ARRAY(array) (sizeof(array) / sizeof(array[0]))
#define DIV_ROUNDUP(a, b) (((a) + ((b) - 1)) / (b))

// The loader enumerates at most this many; keep the installer in step.
#define MAX_GPT_PARTITIONS 256

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
    uint64_t disk_guid[2];

    // entries related
    uint64_t partition_entry_lba;
    uint32_t number_of_partition_entries;
    uint32_t size_of_partition_entry;
    uint32_t partition_entry_array_crc32;
};

struct gpt_entry {
    uint64_t partition_type_guid[2];

    uint64_t unique_partition_guid[2];

    uint64_t starting_lba;
    uint64_t ending_lba;

    uint64_t attributes;

    uint16_t partition_name[36];
};

struct gpt2mbr_type_conv {
    uint64_t gpt_type1;
    uint64_t gpt_type2;
    uint8_t mbr_type;
};

// This table is very incomplete, but it should be enough for covering
// all that matters for ISOHYBRIDs.
// Of course, though, expansion is welcome.
static struct gpt2mbr_type_conv gpt2mbr_type_conv_table[] = {
    { 0x11d2f81fc12a7328, 0x3bc93ec9a0004bba, 0xef }, // EFI system partition
    { 0x4433b9e5ebd0a0a2, 0xc79926b7b668c087, 0x07 }, // Microsoft basic data
    { 0x11aa000048465300, 0xacec4365300011aa, 0xaf }, // HFS/HFS+
};

static int gpt2mbr_type(uint64_t gpt_type1, uint64_t gpt_type2) {
    for (size_t i = 0; i < SIZEOF_ARRAY(gpt2mbr_type_conv_table); i++) {
        if (gpt2mbr_type_conv_table[i].gpt_type1 == gpt_type1
         && gpt2mbr_type_conv_table[i].gpt_type2 == gpt_type2) {
            return gpt2mbr_type_conv_table[i].mbr_type;
        }
    }
    return -1;
}

static void lba2chs(uint8_t *chs, uint64_t lba) {
    // If LBA is too big to express, use a standard value for CHS.
    if (lba > 63 * 255 * 1024) {
        goto lba_too_big;
    }

    uint64_t cylinder = lba / (255 * 63);
    if (cylinder >= 1024) {
lba_too_big:
        chs[0] = 0xfe;
        chs[1] = 0xff;
        chs[2] = 0xff;
        return;
    }
    uint64_t head = (lba / 63) % 255;
    uint64_t sector = (lba % 63) + 1;

    chs[0] = head;
    chs[1] = (cylinder >> 2) & 0xc0; // high 2 bits
    chs[1] |= sector & 0x3f;
    chs[2] = cylinder; // low 8 bits
}

static uint16_t endswap16(uint16_t value) {
    uint16_t ret = 0;
    ret |= (value >> 8) & 0x00ff;
    ret |= (value << 8) & 0xff00;
    return ret;
}

static uint32_t endswap32(uint32_t value) {
    uint32_t ret = 0;
    ret |= (value >> 24) & 0x000000ff;
    ret |= (value >> 8)  & 0x0000ff00;
    ret |= (value << 8)  & 0x00ff0000;
    ret |= (value << 24) & 0xff000000;
    return ret;
}

static uint64_t endswap64(uint64_t value) {
    uint64_t ret = 0;
    ret |= (value >> 56) & 0x00000000000000ff;
    ret |= (value >> 40) & 0x000000000000ff00;
    ret |= (value >> 24) & 0x0000000000ff0000;
    ret |= (value >> 8)  & 0x00000000ff000000;
    ret |= (value << 8)  & 0x000000ff00000000;
    ret |= (value << 24) & 0x0000ff0000000000;
    ret |= (value << 40) & 0x00ff000000000000;
    ret |= (value << 56) & 0xff00000000000000;
    return ret;
}

#ifdef __BYTE_ORDER__

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define bigendian true
#else
#define bigendian false
#endif

#else /* !__BYTE_ORDER__ */

static bool bigendian = false;

#endif /* !__BYTE_ORDER__ */

#define ENDSWAP(VALUE) (bigendian ? (                    \
    sizeof(VALUE) == 1 ? (VALUE)          :              \
    sizeof(VALUE) == 2 ? endswap16(VALUE) :              \
    sizeof(VALUE) == 4 ? endswap32(VALUE) :              \
    sizeof(VALUE) == 8 ? endswap64(VALUE) : (abort(), 1) \
) : (VALUE))

static enum {
    CACHE_CLEAN,
    CACHE_DIRTY
} cache_state;
static uint64_t cached_block;
static uint8_t *cache  = NULL;
static FILE    *device = NULL;
static size_t   block_size;

static bool device_init(void) {
    size_t guesses[] = { 512, 2048, 4096 };

    for (size_t i = 0; i < SIZEOF_ARRAY(guesses); i++) {
        void *tmp = realloc(cache, guesses[i]);
        if (tmp == NULL) {
            perror_wrap("error: device_init(): realloc()");
            return false;
        }
        cache = tmp;

        rewind(device);

        size_t ret = fread(cache, guesses[i], 1, device);
        if (ret != 1) {
            continue;
        }

        block_size = guesses[i];

        if (!quiet) {
            fprintf(stderr, "Physical block size of %zu bytes.\n", block_size);
        }

        cache_state  = CACHE_CLEAN;
        cached_block = 0;
        return true;
    }

    fprintf(stderr, "error: device_init(): Couldn't determine block size of device.\n");
    return false;
}

static bool device_flush_cache(void) {
    if (cache_state == CACHE_CLEAN)
        return true;

    if (set_pos(device, cached_block * block_size) != 0) {
        perror_wrap("error: device_flush_cache(): set_pos()");
        return false;
    }

    size_t ret = fwrite(cache, block_size, 1, device);
    if (ret != 1) {
        if (ferror(device)) {
            perror_wrap("error: device_flush_cache(): fwrite()");
        }
        return false;
    }

    // fwrite() only fills the stdio buffer; the block does not reach the host
    // environment until this returns.
    if (fflush(device) != 0) {
        perror_wrap("error: device_flush_cache(): fflush()");
        return false;
    }

    cache_state = CACHE_CLEAN;
    return true;
}

static bool device_cache_block(uint64_t block) {
    if (cached_block == block)
        return true;

    if (cache_state == CACHE_DIRTY) {
        if (!device_flush_cache())
            return false;
    }

    if (set_pos(device, block * block_size) != 0) {
        perror_wrap("error: device_cache_block(): set_pos()");
        return false;
    }

    // A short read still copies what it got.
    cached_block = (uint64_t)-1;

    size_t ret = fread(cache, block_size, 1, device);
    if (ret != 1) {
        if (ferror(device)) {
            perror_wrap("error: device_cache_block(): fread()");
        }
        return false;
    }

    cached_block = block;

    return true;
}

struct uninstall_data {
    void *data;
    uint64_t loc;
    uint64_t count;
};

#define UNINSTALL_DATA_MAX 256

static bool uninstalling = false;
static struct uninstall_data uninstall_data[UNINSTALL_DATA_MAX];
static struct uninstall_data uninstall_data_rev[UNINSTALL_DATA_MAX];
static uint64_t uninstall_data_i = 0;
static const char *uninstall_file = NULL;

static void reverse_uninstall_data(void) {
    for (size_t i = 0, j = uninstall_data_i - 1; i < uninstall_data_i; i++, j--) {
        uninstall_data_rev[j] = uninstall_data[i];
    }

    memcpy(uninstall_data, uninstall_data_rev, uninstall_data_i * sizeof(struct uninstall_data));
}

static void free_uninstall_data(void) {
    for (size_t i = 0; i < uninstall_data_i; i++) {
        free(uninstall_data[i].data);
    }
}

static bool store_uninstall_data(const char *filename) {
    if (!quiet) {
        fprintf(stderr, "Storing uninstall data to file: `%s`...\n", filename);
    }

    FILE *udfile = fopen(filename, "wb");
    if (udfile == NULL) {
        perror_wrap("error: `%s`", filename);
        goto error;
    }

    if (fwrite(&uninstall_data_i, sizeof(uint64_t), 1, udfile) != 1) {
        goto fwrite_error;
    }

    for (size_t i = 0; i < uninstall_data_i; i++) {
        if (fwrite(&uninstall_data[i].loc, sizeof(uint64_t), 1, udfile) != 1) {
            goto fwrite_error;
        }
        if (fwrite(&uninstall_data[i].count, sizeof(uint64_t), 1, udfile) != 1) {
            goto fwrite_error;
        }
        if (fwrite(uninstall_data[i].data, uninstall_data[i].count, 1, udfile) != 1) {
            goto fwrite_error;
        }
    }

    // A buffered write can fail here rather than at the fwrite that queued it.
    if (fclose(udfile) != 0) {
        perror_wrap("error: store_uninstall_data(): fclose()");
        return false;
    }

    return true;

fwrite_error:
    perror_wrap("error: store_uninstall_data(): fwrite()");

error:
    if (udfile != NULL) {
        fclose(udfile);
    }
    return false;
}

static bool load_uninstall_data(const char *filename) {
    size_t loaded_count = 0;
    uint64_t count = 0;

    if (!quiet) {
        fprintf(stderr, "Loading uninstall data from file: `%s`...\n", filename);
    }

    FILE *udfile = fopen(filename, "rb");
    if (udfile == NULL) {
        perror_wrap("error: `%s`", filename);
        goto error;
    }

    // A short read still copies what it got, so the count stays local until the
    // whole file has loaded: free_uninstall_data() walks whatever is published.
    if (fread(&count, sizeof(uint64_t), 1, udfile) != 1) {
        goto fread_error;
    }

    if (count > UNINSTALL_DATA_MAX) {
        fprintf(stderr, "error: load_uninstall_data(): too many entries (%zu > %d)\n",
                (size_t)count, UNINSTALL_DATA_MAX);
        goto error;
    }

    for (size_t i = 0; i < count; i++) {
        if (fread(&uninstall_data[i].loc, sizeof(uint64_t), 1, udfile) != 1) {
            goto fread_error;
        }
        if (fread(&uninstall_data[i].count, sizeof(uint64_t), 1, udfile) != 1) {
            goto fread_error;
        }
        if (uninstall_data[i].count > SIZE_MAX) {
            fprintf(stderr, "error: load_uninstall_data(): entry size too large\n");
            goto error;
        }
        uninstall_data[i].data = malloc((size_t)uninstall_data[i].count);
        if (uninstall_data[i].data == NULL) {
            perror_wrap("error: load_uninstall_data(): malloc()");
            goto error;
        }
        if (fread(uninstall_data[i].data, uninstall_data[i].count, 1, udfile) != 1) {
            free(uninstall_data[i].data);
            goto fread_error;
        }
        loaded_count++;
    }

    uninstall_data_i = count;

    fclose(udfile);
    return true;

fread_error:
    perror_wrap("error: load_uninstall_data(): fread()");

error:
    // Free any previously allocated uninstall data
    for (size_t j = 0; j < loaded_count; j++) {
        free(uninstall_data[j].data);
    }
    if (udfile != NULL) {
        fclose(udfile);
    }
    return false;
}

static bool device_read_raw(void *_buffer, uint64_t loc, size_t count) {
    uint8_t *buffer = _buffer;
    uint64_t progress = 0;
    while (progress < count) {
        uint64_t block = (loc + progress) / block_size;

        if (!device_cache_block(block)) {
            return false;
        }

        uint64_t chunk = count - progress;
        uint64_t offset = (loc + progress) % block_size;
        if (chunk > block_size - offset)
            chunk = block_size - offset;

        memcpy(buffer + progress, &cache[offset], chunk);
        progress += chunk;
    }

    return true;
}

static bool device_write_raw(const void *_buffer, uint64_t loc, size_t count) {
    struct uninstall_data *ud = NULL;

    if (uninstalling) {
        goto skip_save;
    }

    if (uninstall_data_i >= UNINSTALL_DATA_MAX) {
        fprintf(stderr, "error: Too many uninstall data entries! Please report this bug upstream.\n");
        return false;
    }

    ud = &uninstall_data[uninstall_data_i];

    ud->data = malloc(count);
    if (ud->data == NULL) {
        perror_wrap("error: device_write_raw(): malloc()");
        return false;
    }

    if (!device_read_raw(ud->data, loc, count)) {
        free(ud->data);
        ud->data = NULL;
        return false;
    }

    ud->loc = loc;
    ud->count = count;

skip_save:;
    const uint8_t *buffer = _buffer;
    uint64_t progress = 0;
    while (progress < count) {
        uint64_t block = (loc + progress) / block_size;

        if (!device_cache_block(block)) {
            if (!uninstalling) {
                free(ud->data);
                ud->data = NULL;
            }
            return false;
        }

        uint64_t chunk = count - progress;
        uint64_t offset = (loc + progress) % block_size;
        if (chunk > block_size - offset)
            chunk = block_size - offset;

        memcpy(&cache[offset], buffer + progress, chunk);
        cache_state = CACHE_DIRTY;
        progress += chunk;
    }

    if (!uninstalling) {
        uninstall_data_i++;
    }
    return true;
}

static bool uninstall(bool quiet_arg) {
    bool print_cache_flush_fail = false;
    bool print_write_fail = false;
    bool ret = true;

    uninstalling = true;

    cache_state = CACHE_CLEAN;
    cached_block = (uint64_t)-1;

    for (size_t i = 0; i < uninstall_data_i; i++) {
        struct uninstall_data *ud = &uninstall_data[i];
        bool retry = false;
        while (!device_write_raw(ud->data, ud->loc, ud->count)) {
            if (retry) {
                fprintf(stderr, "warning: Retry failed.\n");
                print_write_fail = true;
                break;
            }
            if (!quiet) {
                fprintf(stderr, "warning: Uninstall data index %zu failed to write, retrying...\n", i);
            }
            if (!device_flush_cache()) {
                print_cache_flush_fail = true;
            }
            cache_state = CACHE_CLEAN;
            cached_block = (uint64_t)-1;
            retry = true;
        }
    }

    if (!device_flush_cache()) {
        print_cache_flush_fail = true;
    }

    if (print_write_fail) {
        fprintf(stderr, "error: Some data failed to be uninstalled correctly.\n");
        ret = false;
    }

    if (print_cache_flush_fail) {
        fprintf(stderr, "error: Device cache flush failure. Uninstall may be incomplete.\n");
        ret = false;
    }

    if (ret == true && !quiet && !quiet_arg) {
        fprintf(stderr, "Uninstall data restored successfully.\n");
    }

    return ret;
}

#define device_read(BUFFER, LOC, COUNT)        \
    do {                                       \
        if (!device_read_raw(BUFFER, LOC, COUNT)) \
            goto cleanup;                      \
    } while (0)

#define device_write(BUFFER, LOC, COUNT)        \
    do {                                        \
        if (!device_write_raw(BUFFER, LOC, COUNT)) \
            goto cleanup;                       \
    } while (0)

static void bios_install_usage(void) {
    printf("usage: %s bios-install <device> [GPT partition index]\n", program_name);
    printf("\n");
    printf("    --force         Force installation even if the safety checks fail\n");
    printf("                    (DANGEROUS!)\n");
    printf("\n");
    printf("    --uninstall     Reverse the entire install procedure\n");
    printf("\n");
    printf("    --uninstall-data-file=<filename>\n");
    printf("                    Set the input (for --uninstall) or output file\n");
    printf("                    name of the file which contains uninstall data\n");
    printf("\n");
    printf("    --no-gpt-to-mbr-isohybrid-conversion\n");
    printf("                    Do not automatically convert a GUID partition table (GPT)\n");
    printf("                    found on an ISOHYBRID image into an MBR partition table\n");
    printf("                    (which is done for better hardware compatibility)\n");
    printf("\n");
    printf("    --quiet         Do not print verbose diagnostic messages\n");
    printf("\n");
    printf("    --help | -h     Display this help message\n");
    printf("\n");
}

static bool validate_or_force(uint64_t offset, bool force, bool *err) {
    *err = false;

    char hintc[64];
    device_read(hintc, offset + 3, 4);
    if (memcmp(hintc, "NTFS", 4) == 0) {
        if (!force) {
            return false;
        } else {
            memset(hintc, 0, 4);
            device_write(hintc, offset + 3, 4);
        }
    }
    device_read(hintc, offset + 54, 3);
    if (memcmp(hintc, "FAT", 3) == 0) {
        if (!force) {
            return false;
        } else {
            memset(hintc, 0, 5);
            device_write(hintc, offset + 54, 5);
        }
    }
    device_read(hintc, offset + 82, 3);
    if (memcmp(hintc, "FAT", 3) == 0) {
        if (!force) {
            return false;
        } else {
            memset(hintc, 0, 5);
            device_write(hintc, offset + 82, 5);
        }
    }
    device_read(hintc, offset + 3, 5);
    if (memcmp(hintc, "FAT32", 5) == 0) {
        if (!force) {
            return false;
        } else {
            memset(hintc, 0, 5);
            device_write(hintc, offset + 3, 5);
        }
    }
    uint16_t hint16 = 0;
    device_read(&hint16, offset + 1080, sizeof(uint16_t));
    hint16 = ENDSWAP(hint16);
    if (hint16 == 0xef53) {
        if (!force) {
            return false;
        } else {
            hint16 = 0;
            hint16 = ENDSWAP(hint16);
            device_write(&hint16, offset + 1080, sizeof(uint16_t));
        }
    }

    return true;

cleanup:
    *err = true;
    return false;
}

#define GPT_HEADER_SIZE 92
#define GPT_HEADER_CRC_OFFSET 16

// A resource limit, not a conformance one: the specification states no maximum,
// and the geometry that does bound the array is written by the same table. 64
// times the 16384 bytes UEFI requires be reserved.
#define GPT_MAX_ARRAY_SIZE (1024 * 1024)

// Bitwise: this runs a handful of times per install, so a table would cost more
// space than the loop costs time.
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

// Read from the device rather than taken from the struct, whose members C is
// free to pad, and with the CRC's own field zeroed the way the CRC was formed.
static bool gpt_header_crc(uint64_t loc, uint32_t header_size, uint32_t *out) {
    uint8_t chunk[512];
    uint32_t crc = 0xffffffff;
    uint32_t done = 0;

    while (done < header_size) {
        uint32_t step = header_size - done;
        if (step > sizeof(chunk)) {
            step = sizeof(chunk);
        }

        if (!device_read_raw(chunk, loc + done, step)) {
            return false;
        }

        for (uint32_t i = 0; i < step; i++) {
            if (done + i >= GPT_HEADER_CRC_OFFSET
             && done + i < GPT_HEADER_CRC_OFFSET + 4) {
                chunk[i] = 0;
            }
        }

        crc = crc32_update(crc, chunk, step);
        done += step;
    }

    *out = ~crc;
    return true;
}

static bool gpt_entry_array_crc(uint64_t loc, uint64_t size, uint32_t *out) {
    uint8_t chunk[512];
    uint32_t crc = 0xffffffff;

    while (size > 0) {
        size_t step = size < sizeof(chunk) ? (size_t)size : sizeof(chunk);

        if (!device_read_raw(chunk, loc, step)) {
            return false;
        }

        crc = crc32_update(crc, chunk, step);
        loc += step;
        size -= step;
    }

    *out = ~crc;
    return true;
}

// UEFI 2.11 section 5.3.2 requires four checks before a GPT may be used: the
// signature, the header CRC, that MyLBA names the block the header was read
// from, and the entry array CRC.
static bool gpt_verify_header(const struct gpt_table_header *header,
                              uint64_t header_lba, uint64_t lb_size,
                              uint64_t device_blocks, uint64_t *budget) {
    uint32_t header_size, entry_size, crc;
    uint64_t header_loc, array_loc, array_size;

    if (strncmp(header->signature, "EFI PART", 8) != 0) {
        return false;
    }

    if (ENDSWAP(header->revision) != 0x00010000) {
        return false;
    }

    header_size = ENDSWAP(header->header_size);
    if (header_size < GPT_HEADER_SIZE || (uint64_t)header_size > lb_size) {
        return false;
    }

    if (ENDSWAP(header->my_lba) != header_lba) {
        return false;
    }

    if (mul_u64_overflow(header_lba, lb_size, &header_loc)) {
        return false;
    }

    if (!gpt_header_crc(header_loc, header_size, &crc)
     || crc != ENDSWAP(header->crc32)) {
        return false;
    }

    // "shall be set to a value of 128 x 2^n", which is to say a power of two no
    // smaller than an entry. Revisions before 2.8 allowed any multiple of 8.
    entry_size = ENDSWAP(header->size_of_partition_entry);
    if (entry_size < sizeof(struct gpt_entry)
     || (entry_size & (entry_size - 1)) != 0) {
        return false;
    }

    if (mul_u64_overflow(ENDSWAP(header->number_of_partition_entries),
                         entry_size, &array_size)) {
        return false;
    }

    if (array_size == 0 || array_size > GPT_MAX_ARRAY_SIZE
     || array_size > *budget) {
        return false;
    }

    // The array is reserved outside the usable range: it precedes FirstUsableLBA
    // on the primary, and follows LastUsableLBA and precedes its own header on
    // the alternate.
    uint64_t array_lba = ENDSWAP(header->partition_entry_lba);
    uint64_t array_blocks = (array_size + lb_size - 1) / lb_size;
    uint64_t array_end;

    if (add_u64_overflow(array_lba, array_blocks, &array_end)) {
        return false;
    }

    uint64_t first_usable = ENDSWAP(header->first_usable_lba);
    uint64_t last_usable = ENDSWAP(header->last_usable_lba);

    if (first_usable > last_usable) {
        return false;
    }

    if (array_lba < first_usable) {
        if (array_end > first_usable) {
            return false;
        }
    } else if (array_lba <= last_usable || array_end > header_lba) {
        return false;
    }

    // Only the array has to be readable: LastUsableLBA is the table's claim
    // about the medium, and where a partition really overruns the device it is
    // refused where it is used.
    if (device_blocks != 0 && array_end > device_blocks) {
        return false;
    }

    if (mul_u64_overflow(array_lba, lb_size, &array_loc)) {
        return false;
    }

    *budget -= array_size;

    return gpt_entry_array_crc(array_loc, array_size, &crc)
        && crc == ENDSWAP(header->partition_entry_array_crc32);
}

// Probed rather than read from AlternateLBA, because a header that failed its
// own CRC cannot be trusted to say where its alternate lives.
// The last byte, not the first: a medium ending mid-block carries no such block,
// and the loader counts blocks by dividing the medium rather than by probing.
static bool device_block_present(uint64_t block, uint64_t lb_size) {
    uint8_t probe;
    uint64_t loc;

    if (mul_u64_overflow(block, lb_size, &loc)
     || add_u64_overflow(loc, lb_size - 1, &loc)) {
        return false;
    }

    return device_read_raw(&probe, loc, 1);
}

static bool device_last_block(uint64_t lb_size, uint64_t *out) {
    uint64_t lo = 0, hi = 1;

    if (!device_block_present(0, lb_size)) {
        return false;
    }

    for (;;) {
        if (!device_block_present(hi, lb_size)) {
            break;
        }

        lo = hi;
        if (hi > UINT64_MAX / 2) {
            return false;
        }
        hi *= 2;
    }

    while (lo + 1 < hi) {
        uint64_t mid = lo + (hi - lo) / 2;

        if (device_block_present(mid, lb_size)) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    *out = lo;
    return true;
}

// A hybrid MBR carries its 0xEE entry beside the real ones, so it counts too.
static bool gpt_protective_mbr(void) {
    for (int i = 0; i < 4; i++) {
        uint8_t type;

        if (!device_read_raw(&type, 0x1be + 16 * i + 4, sizeof(type))) {
            return false;
        }

        if (type == 0xee) {
            return true;
        }
    }

    return false;
}

// UEFI 2.11 section 5.3.2 requires falling back to the alternate header when the
// primary does not verify, and places it in the last block. A disk imaged onto a
// larger one keeps its alternate where the smaller one ended, so the block the
// primary names is tried after the last block rather than instead of it: a
// genuine alternate at the end wins over whatever a corrupt primary points at.
static bool gpt_locate_header(struct gpt_table_header *header,
                              uint64_t *lb_size_out, uint64_t *header_lba_out) {
    // Probed, not taken from the device: the size a table was written for
    // belongs to the image. 2048 is optical, and matches device_init().
    uint64_t lb_guesses[] = { 512, 2048, 4096 };
    bool protective = gpt_protective_mbr();
    // A header that fails its array CRC has already paid for it, so the budget
    // covers the two locations the recovery rule names rather than one call.
    uint64_t budget = GPT_MAX_ARRAY_SIZE * 2;

    for (size_t i = 0; i < SIZEOF_ARRAY(lb_guesses); i++) {
        uint64_t lb_size = lb_guesses[i], last, loc, device_blocks = 0;
        uint64_t candidates[2];
        size_t candidate_count = 0, j;
        bool have_last = device_last_block(lb_size, &last);

        if (have_last) {
            device_blocks = last + 1;
        }

        if (device_read_raw(header, lb_size, sizeof(*header))) {
            if (gpt_verify_header(header, 1, lb_size, device_blocks, &budget)) {
                *lb_size_out = lb_size;
                *header_lba_out = 1;
                return true;
            }

            // The signature is what identifies the block as a header at all,
            // so only a header that failed its CRC is followed. Without it the
            // field is not an LBA, it is whatever happens to be at offset 32.
            if (strncmp(header->signature, "EFI PART", 8) == 0
             && ENDSWAP(header->alternate_lba) > 1) {
                candidates[candidate_count++] = ENDSWAP(header->alternate_lba);
            }
        }

        if (have_last && last >= 1) {
            if (candidate_count > 0 && candidates[0] == last) {
                candidate_count = 0;
            }
            candidates[candidate_count++] = last;
            if (candidate_count == 2) {
                uint64_t claimed = candidates[0];
                candidates[0] = candidates[1];
                candidates[1] = claimed;
            }
        }

        // A disk reformatted to MBR keeps a backup header the new table did not
        // reach, and LBA 0 is what says whether that header is still live.
        if (!protective) {
            continue;
        }

        for (j = 0; j < candidate_count; j++) {
            if (mul_u64_overflow(candidates[j], lb_size, &loc)) {
                continue;
            }

            if (device_read_raw(header, loc, sizeof(*header))
             && gpt_verify_header(header, candidates[j], lb_size, device_blocks, &budget)) {
                *lb_size_out = lb_size;
                *header_lba_out = candidates[j];
                return true;
            }
        }
    }

    return false;
}

static int bios_install(int argc, char *argv[]) {
    int ok = EXIT_FAILURE;
    bool force = false;
    bool gpt2mbr_allowed = true;
    bool uninstall_mode = false;
    const uint8_t *bootloader_img = binary_limine_hdd_bin_data;
    size_t   bootloader_file_size = sizeof(binary_limine_hdd_bin_data);
    uint8_t  orig_mbr[70], timestamp[6];
    void *empty_lba = NULL;
    const char *part_ndx = NULL;

#ifndef __BYTE_ORDER__
    uint32_t endcheck = 0x12345678;
    unsigned char endbyte = *((unsigned char *)&endcheck);
    bigendian = endbyte == 0x12;
#endif

    if (argc < 2) {
        bios_install_usage();
#ifdef IS_WINDOWS
        system("pause");
#endif
        return EXIT_FAILURE;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            bios_install_usage();
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(argv[i], "--force") == 0) {
            if (force && !quiet) {
                fprintf(stderr, "warning: --force already set.\n");
            }
            force = true;
        } else if (strcmp(argv[i], "--no-gpt-to-mbr-isohybrid-conversion") == 0) {
            gpt2mbr_allowed = false;
        } else if (strcmp(argv[i], "--uninstall") == 0) {
            if (uninstall_mode && !quiet) {
                fprintf(stderr, "warning: --uninstall already set.\n");
            }
            uninstall_mode = true;
        } else if (strncmp(argv[i], "--uninstall-data-file=", 22) == 0) {
            if (uninstall_file != NULL && !quiet) {
                fprintf(stderr, "warning: --uninstall-data-file already set. Overriding...\n");
            }
            uninstall_file = argv[i] + 22;
            if (strlen(uninstall_file) == 0) {
                fprintf(stderr, "error: Uninstall data file has a zero-length name!\n");
                return EXIT_FAILURE;
            }
        } else {
            if (device != NULL) { // [GPT partition index]
                part_ndx = argv[i]; // TODO: Make this non-positional?
            } else if ((device = fopen(argv[i], "r+b")) == NULL) { // <device>
                perror_wrap("error: `%s`", argv[i]);
                return EXIT_FAILURE;
            }
        }
    }

    if (device == NULL) {
        fprintf(stderr, "error: No device specified\n");
        bios_install_usage();
        return EXIT_FAILURE;
    }

    if (!device_init()) {
        goto uninstall_mode_cleanup;
    }

    if (uninstall_mode) {
        if (uninstall_file == NULL) {
            fprintf(stderr, "error: Uninstall mode set but no --uninstall-data-file=... passed.\n");
            goto uninstall_mode_cleanup;
        }

        if (!load_uninstall_data(uninstall_file)) {
            goto uninstall_mode_cleanup;
        }

        if (uninstall(false) == false) {
            ok = EXIT_FAILURE;
        } else {
            ok = EXIT_SUCCESS;
        }
        goto uninstall_mode_cleanup;
    }

    // Probe for GPT and logical block size
    int gpt = 0;
    struct gpt_table_header gpt_header;
    uint64_t lb_size = 0;
    uint64_t gpt_header_lba = 0;
    bool gpt_from_alternate = false;
    uint32_t gpt_entry_count = 0;

    if (gpt_locate_header(&gpt_header, &lb_size, &gpt_header_lba)) {
        gpt_from_alternate = gpt_header_lba != 1;
        gpt = 1;
        gpt_entry_count = ENDSWAP(gpt_header.number_of_partition_entries);
        if (gpt_entry_count > MAX_GPT_PARTITIONS) {
            gpt_entry_count = MAX_GPT_PARTITIONS;
        }
        if (!quiet) {
            fprintf(stderr, "Installing to GPT. Logical block size of %" PRIu64 " bytes.\n",
                    lb_size);
            if (gpt_from_alternate) {
                fprintf(stderr, "warning: Primary GPT did not verify; using the alternate header.\n");
            }
        }
    }

    // Check if this is an ISO w/ a GPT, in which case try converting it
    // to MBR for improved compatibility with a whole range of hardware that
    // does not like booting off of GPT in BIOS or CSM mode, and other
    // broken hardware.
    if (gpt && gpt2mbr_allowed == true) {
        char iso_signature[5];
        device_read(iso_signature, 32769, 5);

        if (strncmp(iso_signature, "CD001", 5) != 0) {
            goto no_mbr_conv;
        }

        if (!quiet) {
            fprintf(stderr, "Detected ISOHYBRID with a GUID partition table (GPT).\n");
            fprintf(stderr, "Converting to MBR for improved compatibility...\n");
        }

        // Gather the (up to 4) GPT partition to convert.
        struct {
            uint64_t lba_start;
            uint64_t lba_end;
            uint8_t chs_start[3];
            uint8_t chs_end[3];
            uint8_t type;
        } part_to_conv[4];
        size_t part_to_conv_i = 0;

        // The cap bounds the work, so a table declaring more entries than it
        // is one the loop below cannot examine in full: the refusals it makes
        // per entry would silently not cover the rest.
        if (ENDSWAP(gpt_header.number_of_partition_entries) > MAX_GPT_PARTITIONS) {
            if (!quiet) {
                fprintf(stderr, "GPT declares more than %d partition entries, will not convert GPT.\n",
                        MAX_GPT_PARTITIONS);
            }
            goto no_mbr_conv;
        }

        uint64_t part_entry_base;
        if (mul_u64_overflow(ENDSWAP(gpt_header.partition_entry_lba), lb_size, &part_entry_base)) {
            goto no_mbr_conv;
        }

        // The converted entries describe the medium, and LastUsableLBA is the
        // table's claim about it rather than a measurement of it.
        uint64_t conv_last_block;
        if (!device_last_block(lb_size, &conv_last_block)) {
            if (!quiet) {
                fprintf(stderr, "Could not determine the size of the device, will not convert GPT.\n");
            }
            goto no_mbr_conv;
        }

        for (int64_t i = 0; i < (int64_t)gpt_entry_count; i++) {
            struct gpt_entry gpt_entry;
            uint64_t entry_offset = (uint64_t)i * ENDSWAP(gpt_header.size_of_partition_entry);
            if (add_u64_overflow(part_entry_base, entry_offset, &entry_offset)) {
                goto no_mbr_conv;
            }
            device_read(&gpt_entry, entry_offset, sizeof(struct gpt_entry));

            if (gpt_entry.partition_type_guid[0] == 0 &&
                gpt_entry.partition_type_guid[1] == 0) {
                continue;
            }

            if (part_to_conv_i == 4) {
                if (!quiet) {
                    fprintf(stderr, "GPT contains more than 4 partitions, will not convert.\n");
                }
                goto no_mbr_conv;
            }

            // An MBR entry counts 512-byte sectors while a GPT entry counts
            // logical blocks, so the two agree only on a 512-byte device.
            uint64_t start_lba = ENDSWAP(gpt_entry.starting_lba);
            uint64_t end_lba = ENDSWAP(gpt_entry.ending_lba);
            uint64_t start_sect, sect_count;

            if (end_lba < start_lba) {
                if (!quiet) {
                    fprintf(stderr, "Partition %" PRIi64 " ends before it starts, will not convert GPT.\n", i + 1);
                }
                goto no_mbr_conv;
            }

            // The erase stops at FirstUsableLBA and resumes past
            // LastUsableLBA, so it cannot reach a partition inside the range.
            if (start_lba < ENDSWAP(gpt_header.first_usable_lba)
             || end_lba > ENDSWAP(gpt_header.last_usable_lba)) {
                if (!quiet) {
                    fprintf(stderr, "Partition %" PRIi64 " lies outside the GPT usable range, will not convert GPT.\n", i + 1);
                }
                goto no_mbr_conv;
            }

            if (end_lba > conv_last_block) {
                if (!quiet) {
                    fprintf(stderr, "Partition %" PRIi64 " ends past the device, will not convert GPT.\n", i + 1);
                }
                goto no_mbr_conv;
            }

            if (mul_u64_overflow(start_lba, lb_size / 512, &start_sect)
             || start_sect > UINT32_MAX) {
                if (!quiet) {
                    fprintf(stderr, "Starting LBA of partition %" PRIi64 " is greater than UINT32_MAX, will not convert GPT.\n", i + 1);
                }
                goto no_mbr_conv;
            }

            if (mul_u64_overflow(end_lba - start_lba + 1, lb_size / 512, &sect_count)
             || sect_count > UINT32_MAX
             || start_sect + sect_count - 1 > UINT32_MAX) {
                if (!quiet) {
                    fprintf(stderr, "Sector count of partition %" PRIi64 " is greater than UINT32_MAX, will not convert GPT.\n", i + 1);
                }
                goto no_mbr_conv;
            }

            part_to_conv[part_to_conv_i].lba_start = start_sect;
            part_to_conv[part_to_conv_i].lba_end = start_sect + sect_count - 1;
            lba2chs(part_to_conv[part_to_conv_i].chs_start, part_to_conv[part_to_conv_i].lba_start);
            lba2chs(part_to_conv[part_to_conv_i].chs_end, part_to_conv[part_to_conv_i].lba_end);

            int type = gpt2mbr_type(ENDSWAP(gpt_entry.partition_type_guid[0]),
                                    ENDSWAP(gpt_entry.partition_type_guid[1]));
            if (type == -1) {
                if (!quiet) {
                    fprintf(stderr, "Cannot convert partition type for partition %" PRIi64 ", will not convert GPT.\n", i + 1);
                }
                goto no_mbr_conv;
            }

            part_to_conv[part_to_conv_i].type = type;

            part_to_conv_i++;
        }

        // The MBR checks below refuse a start under 63, but only after the
        // conversion has committed. Nothing has been written here yet.
        for (size_t i = 0; i < part_to_conv_i; i++) {
            if (part_to_conv[i].lba_start < 63) {
                goto part_too_low;
            }
        }

        // Nuke the GPTs.
        empty_lba = calloc(1, lb_size);
        if (empty_lba == NULL) {
            perror_wrap("error: bios_install(): malloc()");
            goto cleanup;
        }

        // ... find the alternate GPT the header names and the one at the end of
        // the device: a disk imaged onto a larger one carries both, and leaving
        // either behind is what makes a GPT-aware reader disagree with the MBR
        // this conversion writes.
        uint64_t alternates[2], alt_first[2];
        size_t alternate_count = 0, wipe_count = 0, ai;
        uint64_t last_block;

        // The alternate reserve is the header and the same 16384 bytes, without
        // the protective MBR: 33 blocks at 512, 9 at 2048, 5 at 4096.
        uint64_t alt_reserve = 1 + (16384 + lb_size - 1) / lb_size;

        if (gpt_from_alternate) {
            if (gpt_header_lba >= alt_reserve) {
                alternates[alternate_count++] = gpt_header_lba;
            }
        } else if (ENDSWAP(gpt_header.alternate_lba) >= alt_reserve) {
            alternates[alternate_count++] = ENDSWAP(gpt_header.alternate_lba);
        }

        if (device_last_block(lb_size, &last_block) && last_block >= alt_reserve
         && (alternate_count == 0 || alternates[0] != last_block)) {
            alternates[alternate_count++] = last_block;
        }

        // Settle every erase before performing any of them: a rejection
        // knowable in advance must not depend on the undo succeeding.
        for (ai = 0; ai < alternate_count; ai++) {
            struct gpt_table_header probe;
            uint64_t probe_loc;

            // Checked by signature alone rather than by gpt_verify_header,
            // deliberately: a wrong AlternateLBA reaches nothing, and a table
            // this tool rejects may still be honoured by another reader.
            if (mul_u64_overflow(alternates[ai], lb_size, &probe_loc)
             || !device_read_raw(&probe, probe_loc, sizeof(probe))
             || strncmp(probe.signature, "EFI PART", 8) != 0) {
                continue;
            }

            uint64_t last_usable = ENDSWAP(gpt_header.last_usable_lba);
            uint64_t first = alternates[ai] - (alt_reserve - 1);

            // LastUsableLBA is unbounded by the checks a header must pass, and
            // a table calling its own alternate usable leaves it neither
            // erasable there nor safe to leave behind.
            if (last_usable >= alternates[ai]) {
                fprintf(stderr, "error: GPT places an alternate header inside"
                                " its usable range, aborting.\n");
                goto cleanup;
            }

            if (first <= last_usable) {
                first = last_usable + 1;
            }

            alternates[wipe_count] = alternates[ai];
            alt_first[wipe_count] = first;
            wipe_count++;
        }

        // ... nuke primary GPT + protective MBR. The reserve is the protective
        // MBR, the header, and the 16384 bytes UEFI reserves for the entry
        // array whatever the block size -- 34 blocks at 512, 10 at 2048, 6 at
        // 4096. The header's own value bounds it where that is smaller.
        uint64_t first_usable = ENDSWAP(gpt_header.first_usable_lba);
        uint64_t reserve_max = 2 + (16384 + lb_size - 1) / lb_size;
        uint64_t reserve = first_usable < reserve_max ? first_usable : reserve_max;

        for (uint64_t i = 0; i < reserve; i++) {
            device_write(empty_lba, i * lb_size, lb_size);
        }

        for (ai = 0; ai < wipe_count; ai++) {
            for (uint64_t lba = alt_first[ai]; lba <= alternates[ai]; lba++) {
                uint64_t wipe_loc;
                if (mul_u64_overflow(lba, lb_size, &wipe_loc)) {
                    fprintf(stderr, "error: GPT alternate LBA out of range, aborting.\n");
                    goto cleanup;
                }
                device_write(empty_lba, wipe_loc, lb_size);
            }
        }

        // We're no longer GPT.
        gpt = 0;

        // Derive the MBR disk ID from the GPT disk GUID rather than from the
        // clock: two images converted in the same second would otherwise share
        // an ID, and this keeps the conversion reproducible.
        uint32_t disk_id = 2166136261u;
        const unsigned char *guid = (const unsigned char *)gpt_header.disk_guid;
        for (size_t i = 0; i < sizeof(gpt_header.disk_guid); i++) {
            disk_id = (disk_id ^ guid[i]) * 16777619u;
        }
        if (disk_id == 0) {
            disk_id = 1;
        }
        for (size_t i = 0; i < 4; i++) {
            uint8_t b = (uint8_t)(disk_id >> (i * 8));
            device_write(&b, 0x1b8 + i, 1);
        }

        // Write out the partition entries.
        for (size_t i = 0; i < part_to_conv_i; i++) {
            device_write(&part_to_conv[i].type, 0x1be + i * 16 + 0x04, 1);
            uint32_t lba_start = ENDSWAP((uint32_t)part_to_conv[i].lba_start);
            device_write(&lba_start, 0x1be + i * 16 + 0x08, 4);
            uint32_t sect_count = ENDSWAP((uint32_t)((part_to_conv[i].lba_end - part_to_conv[i].lba_start) + 1));
            device_write(&sect_count, 0x1be + i * 16 + 0x0c, 4);

            device_write(part_to_conv[i].chs_start, 0x1be + i * 16 + 1, 3);
            device_write(part_to_conv[i].chs_end, 0x1be + i * 16 + 5, 3);
        }

        // The protective MBR was wiped above, and its boot signature with it.
        uint16_t mbr_signature = 0xaa55;
        mbr_signature = ENDSWAP(mbr_signature);
        device_write(&mbr_signature, 510, sizeof(uint16_t));

        if (!quiet) {
            fprintf(stderr, "Conversion successful.\n");
        }
    }

no_mbr_conv:;

    int mbr = 0;
    if (gpt == 0) {
        // Do all sanity checks on MBR
        mbr = 1;

        uint8_t hint8 = 0;
        uint16_t hint16 = 0;
        uint32_t hint32 = 0;

        bool any_active = false;

        device_read(&hint16, 510, sizeof(uint16_t));
        hint16 = ENDSWAP(hint16);
        if (hint16 != 0xaa55) {
            if (!force) {
                mbr = 0;
            } else {
                hint16 = 0xaa55;
                hint16 = ENDSWAP(hint16);
                device_write(&hint16, 510, sizeof(uint16_t));
            }
        }

        device_read(&hint8, 446, sizeof(uint8_t));
        if (hint8 != 0x00 && hint8 != 0x80) {
            if (!force) {
                mbr = 0;
            } else {
                hint8 &= 0x80;
                device_write(&hint8, 446, sizeof(uint8_t));
            }
        }
        any_active = any_active || (hint8 & 0x80) != 0;
        device_read(&hint8, 446 + 4, sizeof(uint8_t));
        if (hint8 != 0x00) {
            device_read(&hint32, 446 + 8, sizeof(uint32_t));
            hint32 = ENDSWAP(hint32);
            if (hint32 < 63) {
                goto part_too_low;
            }
        }
        device_read(&hint8, 462, sizeof(uint8_t));
        if (hint8 != 0x00 && hint8 != 0x80) {
            if (!force) {
                mbr = 0;
            } else {
                hint8 &= 0x80;
                device_write(&hint8, 462, sizeof(uint8_t));
            }
        }
        any_active = any_active || (hint8 & 0x80) != 0;
        device_read(&hint8, 462 + 4, sizeof(uint8_t));
        if (hint8 != 0x00) {
            device_read(&hint32, 462 + 8, sizeof(uint32_t));
            hint32 = ENDSWAP(hint32);
            if (hint32 < 63) {
                goto part_too_low;
            }
        }
        device_read(&hint8, 478, sizeof(uint8_t));
        if (hint8 != 0x00 && hint8 != 0x80) {
            if (!force) {
                mbr = 0;
            } else {
                hint8 &= 0x80;
                device_write(&hint8, 478, sizeof(uint8_t));
            }
        }
        any_active = any_active || (hint8 & 0x80) != 0;
        device_read(&hint8, 478 + 4, sizeof(uint8_t));
        if (hint8 != 0x00) {
            device_read(&hint32, 478 + 8, sizeof(uint32_t));
            hint32 = ENDSWAP(hint32);
            if (hint32 < 63) {
                goto part_too_low;
            }
        }
        device_read(&hint8, 494, sizeof(uint8_t));
        if (hint8 != 0x00 && hint8 != 0x80) {
            if (!force) {
                mbr = 0;
            } else {
                hint8 &= 0x80;
                device_write(&hint8, 494, sizeof(uint8_t));
            }
        }
        any_active = any_active || (hint8 & 0x80) != 0;
        device_read(&hint8, 494 + 4, sizeof(uint8_t));
        if (hint8 != 0x00) {
            device_read(&hint32, 494 + 8, sizeof(uint32_t));
            hint32 = ENDSWAP(hint32);
            if (hint32 < 63) {
                goto part_too_low;
            }
        }

        if (0) {
part_too_low:
            fprintf(stderr, "error: A partition's start sector is less than 63, aborting.\n");
            goto cleanup;
        }

        if (mbr) {
            bool err;
            mbr = validate_or_force(0, force, &err);
            if (err) {
                goto cleanup;
            }
        }

        if (mbr && !any_active) {
            if (!quiet) {
                fprintf(stderr, "No active partition found, some systems may not boot.\n");
                fprintf(stderr, "Setting partition 1 as active to work around the issue...\n");
            }
            hint8 = 0x80;
            device_write(&hint8, 446, sizeof(uint8_t));
        }
    }

    if (gpt == 0 && mbr == 0) {
        fprintf(stderr, "error: Could not determine if the device has a valid partition table.\n");
        fprintf(stderr, "       Please ensure the device has a valid MBR or GPT.\n");
        fprintf(stderr, "       Alternatively, pass `--force` to override these checks.\n");
        fprintf(stderr, "       **ONLY DO THIS AT YOUR OWN RISK, DATA LOSS MAY OCCUR!**\n");
        goto cleanup;
    }

    // Default location of stage2 for MBR (in post MBR gap)
    uint64_t stage2_loc = 512;

    // The MBR sanity checks above reject any partition starting before LBA 63,
    // so LBAs 1 through 62 are ours. The GPT path narrows this to the size of
    // the partition it picks.
    uint64_t stage2_max = 62 * 512;

    if (gpt) {
        struct gpt_entry gpt_entry;
        uint32_t partition_num;

        uint64_t gpt_part_entry_base;
        if (mul_u64_overflow(ENDSWAP(gpt_header.partition_entry_lba), lb_size, &gpt_part_entry_base)) {
            fprintf(stderr, "error: GPT partition entry LBA overflows.\n");
            goto cleanup;
        }

        if (part_ndx != NULL) {
            char *part_ndx_end;
            unsigned long part_ndx_val = strtoul(part_ndx, &part_ndx_end, 10);
            if (part_ndx[0] < '0' || part_ndx[0] > '9' || *part_ndx_end != '\0'
             || part_ndx_val == 0 || part_ndx_val > UINT32_MAX) {
                fprintf(stderr, "error: Invalid partition number `%s`: expected a whole"
                                " number starting at 1.\n", part_ndx);
                goto cleanup;
            }
            partition_num = (uint32_t)part_ndx_val - 1;
            if (partition_num >= gpt_entry_count) {
                fprintf(stderr, "error: Partition number is too large.\n");
                goto cleanup;
            }

            uint64_t entry_off = (uint64_t)partition_num * ENDSWAP(gpt_header.size_of_partition_entry);
            if (add_u64_overflow(gpt_part_entry_base, entry_off, &entry_off)) {
                fprintf(stderr, "error: GPT partition entry offset overflows.\n");
                goto cleanup;
            }
            device_read(&gpt_entry, entry_off, sizeof(struct gpt_entry));

            if (gpt_entry.partition_type_guid[0] == 0 &&
              gpt_entry.partition_type_guid[1] == 0) {
                fprintf(stderr, "error: No such partition: %" PRIu32 ".\n", partition_num + 1);
                goto cleanup;
            }

            if (!force && memcmp("Hah!IdontNeedEFI", &gpt_entry.partition_type_guid, 16) != 0) {
                fprintf(stderr, "error: Chosen partition for BIOS boot code is not of BIOS boot partition type.\n");
                fprintf(stderr, "       Pass `--force` to override this check.\n");
                fprintf(stderr, "       **ONLY DO THIS AT YOUR OWN RISK, DATA LOSS MAY OCCUR!**\n");
                goto cleanup;
            }
        } else {
            // Try to autodetect the BIOS boot partition
            for (partition_num = 0; partition_num < gpt_entry_count; partition_num++) {
                uint64_t entry_off = (uint64_t)partition_num * ENDSWAP(gpt_header.size_of_partition_entry);
                if (add_u64_overflow(gpt_part_entry_base, entry_off, &entry_off)) {
                    fprintf(stderr, "error: GPT partition entry offset overflows.\n");
                    goto cleanup;
                }
                device_read(&gpt_entry, entry_off, sizeof(struct gpt_entry));

                if (memcmp("Hah!IdontNeedEFI", &gpt_entry.partition_type_guid, 16) == 0) {
                    if (!quiet) {
                        fprintf(stderr, "Autodetected partition %" PRIu32 " as BIOS boot partition.\n", partition_num + 1);
                    }
                    goto bios_boot_autodetected;
                }
            }

            fprintf(stderr, "error: Installing to a GPT device, but no BIOS boot partition specified or\n");
            fprintf(stderr, "       detected.\n");
            goto cleanup;
        }

bios_boot_autodetected:;
        uint64_t starting_lba = ENDSWAP(gpt_entry.starting_lba);
        uint64_t ending_lba = ENDSWAP(gpt_entry.ending_lba);

        if (ending_lba < starting_lba) {
            fprintf(stderr, "error: Partition %" PRIu32 " has ending LBA less than starting LBA.\n", partition_num + 1);
            goto cleanup;
        }

        // The usable range is the header's own, so a crafted one moves it; the
        // reserve UEFI states is the floor it cannot move.
        if (starting_lba < 2 + (16384 + lb_size - 1) / lb_size) {
            fprintf(stderr, "error: Partition %" PRIu32 " starts inside the GPT reserve.\n", partition_num + 1);
            goto cleanup;
        }

        if (starting_lba < ENDSWAP(gpt_header.first_usable_lba)
         || ending_lba > ENDSWAP(gpt_header.last_usable_lba)) {
            fprintf(stderr, "error: Partition %" PRIu32 " lies outside the GPT usable range.\n", partition_num + 1);
            goto cleanup;
        }

        // The alternate GPT sits at the end of the medium as the primary sits
        // at the start, and no header can move where the medium ends.
        uint64_t last_block;
        if (!device_last_block(lb_size, &last_block)) {
            fprintf(stderr, "error: Could not determine the size of the device.\n");
            goto cleanup;
        }
        uint64_t end_reserve = (16384 + lb_size - 1) / lb_size;
        if (last_block < 1 + end_reserve
         || ending_lba > last_block - 1 - end_reserve) {
            fprintf(stderr, "error: Partition %" PRIu32 " ends inside the alternate GPT.\n", partition_num + 1);
            goto cleanup;
        }

        uint64_t part_size;
        if (mul_u64_overflow(ending_lba - starting_lba + 1, lb_size, &part_size)) {
            fprintf(stderr, "error: Partition %" PRIu32 " size overflows.\n", partition_num + 1);
            goto cleanup;
        }

        if (part_size < 32768) {
            fprintf(stderr, "error: Partition %" PRIu32 " is smaller than 32KiB.\n", partition_num + 1);
            goto cleanup;
        }

        if (mul_u64_overflow(starting_lba, lb_size, &stage2_loc)) {
            fprintf(stderr, "error: Partition %" PRIu32 " starting LBA overflows.\n", partition_num + 1);
            goto cleanup;
        }

        stage2_max = part_size;

        bool err;
        bool valid = validate_or_force(stage2_loc, force, &err);
        if (err) {
            goto cleanup;
        }

        if (!valid) {
            fprintf(stderr, "error: The partition selected to install the BIOS boot code to contains\n");
            fprintf(stderr, "       a recognised filesystem.\n");
            fprintf(stderr, "       Pass `--force` to override these checks.\n");
            fprintf(stderr, "       **ONLY DO THIS AT YOUR OWN RISK, DATA LOSS MAY OCCUR!**\n");
            goto cleanup;
        }

        if (!quiet) {
            fprintf(stderr, "Installing BIOS boot code to partition %" PRIu32 ".\n", partition_num + 1);
        }
    } else {
        if (!quiet) {
            fprintf(stderr, "Installing to MBR.\n");
        }
    }

    if (!quiet) {
        fprintf(stderr, "Stage 2 to be located at byte offset 0x%" PRIx64 ".\n", stage2_loc);
    }

    // Save original timestamp
    device_read(timestamp, 218, 6);

    // Save the original partition table of the device
    device_read(orig_mbr, 440, 70);

    if ((uint64_t)bootloader_file_size - 512 > stage2_max) {
        fprintf(stderr, "error: Stage 2 needs %" PRIu64 " bytes at offset 0x%" PRIx64 ", but only\n",
                (uint64_t)bootloader_file_size - 512, stage2_loc);
        fprintf(stderr, "       %" PRIu64 " are available before the next thing on the device.\n",
                stage2_max);
        goto cleanup;
    }

    // Write the bootsector from the bootloader to the device
    device_write(&bootloader_img[0], 0, 512);

    // Write the rest of stage 2 to the device
    device_write(&bootloader_img[512], stage2_loc, bootloader_file_size - 512);

    // Hardcode in the bootsector the location of stage 2
    stage2_loc = ENDSWAP(stage2_loc);
    device_write(&stage2_loc, 0x1a4, sizeof(uint64_t));

    // Write back timestamp
    device_write(timestamp, 218, 6);

    // Write back the saved partition table to the device
    device_write(orig_mbr, 440, 70);

    if (!device_flush_cache())
        goto cleanup;

    if (!quiet) {
        fprintf(stderr, "Reminder: Remember to copy the limine-bios.sys file in either\n"
                        "          the root, /boot, /limine, or /boot/limine directories of\n"
                        "          one of the partitions on the device, or boot will fail!\n");

        fprintf(stderr, "Limine BIOS stages installed successfully.\n");
    }

    ok = EXIT_SUCCESS;

cleanup:
    reverse_uninstall_data();
    if (ok != EXIT_SUCCESS) {
        // If we failed, attempt to reverse install process
        fprintf(stderr, "Install failed, undoing work...\n");
        uninstall(true);
    } else if (uninstall_file != NULL) {
        store_uninstall_data(uninstall_file);
    }
uninstall_mode_cleanup:
    free_uninstall_data();
    if (empty_lba)
        free(empty_lba);
    if (cache)
        free(cache);
    if (device != NULL) {
        if (fclose(device) != 0) {
            perror_wrap("error: bios_install(): fclose()");
            ok = EXIT_FAILURE;
        }
    }

    return ok;
}
#endif

#define CONFIG_B2SUM_SIGNATURE "++CONFIG_B2SUM_SIGNATURE++"

static void enroll_config_usage(void) {
    printf("usage: %s enroll-config <Limine executable> <BLAKE2B of config file>\n", program_name);
    printf("\n");
    printf("    --reset      Remove enrolled BLAKE2B, will not check config integrity\n");
    printf("\n");
    printf("    --quiet      Do not print verbose diagnostic messages\n");
    printf("\n");
    printf("    --help | -h  Display this help message\n");
    printf("\n");
}

static int enroll_config(int argc, char *argv[]) {
    int ret = EXIT_FAILURE;

    char *bootloader = NULL;
    FILE *bootloader_file = NULL;
    bool quiet_arg = false;
    bool reset = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            enroll_config_usage();
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            remove_arg(&argc, argv, i--);
            quiet_arg = true;
        } else if (strcmp(argv[i], "--reset") == 0) {
            remove_arg(&argc, argv, i--);
            reset = true;
        }
    }

    if (argc <= (reset ? 1 : 2)) {
        enroll_config_usage();
#ifdef IS_WINDOWS
        system("pause");
#endif
        return EXIT_FAILURE;
    }

    if (!reset) {
        if (strlen(argv[2]) != 128) {
            fprintf(stderr, "error: BLAKE2B specified is not 128 characters long.\n");
            goto cleanup;
        }
        for (size_t i = 0; i < 128; i++) {
            char c = argv[2][i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                fprintf(stderr, "error: BLAKE2B specified contains a non-hexadecimal character.\n");
                goto cleanup;
            }
        }
    }

    bootloader_file = fopen(argv[1], "r+b");
    if (bootloader_file == NULL) {
        perror_wrap("error: `%s`", argv[1]);
        goto cleanup;
    }

    if (fseek(bootloader_file, 0, SEEK_END) != 0) {
        perror_wrap("error: enroll_config(): fseek()");
        goto cleanup;
    }
    long ftell_result = ftell(bootloader_file);
    if (ftell_result < 0) {
        perror_wrap("error: enroll_config(): ftell()");
        goto cleanup;
    }
    size_t bootloader_size = (size_t)ftell_result;
    rewind(bootloader_file);

    size_t min_size = (sizeof(CONFIG_B2SUM_SIGNATURE) - 1) + 128;
    if (bootloader_size < min_size) {
        fprintf(stderr, "error: Bootloader file too small (need at least %zu bytes)\n", min_size);
        goto cleanup;
    }

    bootloader = malloc(bootloader_size);
    if (bootloader == NULL) {
        perror_wrap("error: enroll_config(): malloc()");
        goto cleanup;
    }

    if (fread(bootloader, bootloader_size, 1, bootloader_file) != 1) {
        perror_wrap("error: enroll_config(): fread()");
        goto cleanup;
    }

    char *checksum_loc = NULL;
    size_t checked_count = 0;
    const char *config_b2sum_sign = CONFIG_B2SUM_SIGNATURE;
    for (size_t i = 0; i + 128 < bootloader_size; i++) {
        if (bootloader[i] != config_b2sum_sign[checked_count]) {
            if (checked_count > 0) {
                i -= checked_count; // restart after first byte of failed match
                checked_count = 0;
            }
            continue;
        }

        checked_count++;

        if (checked_count == sizeof(CONFIG_B2SUM_SIGNATURE) - 1) {
            checksum_loc = &bootloader[i + 1];
            break;
        }
    }

    if (checksum_loc == NULL) {
        fprintf(stderr, "error: Checksum location not found in provided executable.\n");
        goto cleanup;
    }

    if (!reset) {
        memcpy(checksum_loc, argv[2], 128);
    } else {
        memset(checksum_loc, '0', 128);
    }

    if (fseek(bootloader_file, 0, SEEK_SET) != 0) {
        perror_wrap("error: enroll_config(): fseek()");
        goto cleanup;
    }
    if (fwrite(bootloader, bootloader_size, 1, bootloader_file) != 1) {
        perror_wrap("error: enroll_config(): fwrite()");
        goto cleanup;
    }
    if (fflush(bootloader_file) != 0) {
        perror_wrap("error: enroll_config(): fflush()");
        goto cleanup;
    }

    if (!quiet_arg) {
        fprintf(stderr, "Config file BLAKE2B successfully %s.\n", reset ? "reset" : "enrolled");
    }
    ret = EXIT_SUCCESS;

cleanup:
    if (bootloader != NULL) {
        free(bootloader);
    }
    if (bootloader_file != NULL) {
        if (fclose(bootloader_file) != 0) {
            perror_wrap("error: enroll_config(): fclose()");
            ret = EXIT_FAILURE;
        }
    }
    return ret;
}

#define LIMINE_VERSION "%VERSION%"
#define LIMINE_COPYRIGHT "%COPYRIGHT%"

static void version_usage(void) {
    printf("usage: %s version [options...]\n", program_name);
    printf("\n");
    printf("    --version-only  Only print the version number without licensing info\n");
    printf("                    and other distractions\n");
    printf("\n");
    printf("    --help | -h     Display this help message\n");
    printf("\n");
}

static int version(int argc, char *argv[]) {
    if (argc >= 2) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            version_usage();
            return EXIT_SUCCESS;
        } else if (strcmp(argv[1], "--version-only") == 0) {
            puts(LIMINE_VERSION);
            return EXIT_SUCCESS;
        }
    }

    puts("Limine " LIMINE_VERSION);
    puts(LIMINE_COPYRIGHT);
    puts("Limine is distributed under the terms of the BSD-2-Clause license.");
    puts("There is ABSOLUTELY NO WARRANTY, to the extent permitted by law.");
    return EXIT_SUCCESS;
}

static void general_usage(void) {
    printf("usage: %s <command> <args...>\n", program_name);
    printf("\n");
    printf("    --print-datadir   Print the directory containing the bootloader files\n");
    printf("\n");
    printf("    --version         Print the Limine version (like the `version` command)\n");
    printf("\n");
    printf("    --help | -h       Display this help message\n");
    printf("\n");
    printf("Commands: `help`, `version`, `bios-install`, `enroll-config`\n");
    printf("Use `--help` after specifying the command for command-specific help.\n");
}

static int print_datadir(void) {
#ifdef LIMINE_DATADIR
    puts(LIMINE_DATADIR);
    return EXIT_SUCCESS;
#else
    fprintf(stderr, "error: Cannot print datadir for `limine` built standalone.\n");
    return EXIT_FAILURE;
#endif
}

int main(int argc, char *argv[]) {
    program_name = argv[0];

    if (argc <= 1) {
        general_usage();
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "help") == 0
     || strcmp(argv[1], "--help") == 0
     || strcmp(argv[1], "-h") == 0) {
        general_usage();
        return EXIT_SUCCESS;
    } else if (strcmp(argv[1], "bios-install") == 0) {
#ifndef LIMINE_NO_BIOS
        return bios_install(argc - 1, &argv[1]);
#else
        fprintf(stderr, "error: Limine has been compiled without BIOS support.\n");
        return EXIT_FAILURE;
#endif
    } else if (strcmp(argv[1], "enroll-config") == 0) {
        return enroll_config(argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "--print-datadir") == 0) {
        return print_datadir();
    } else if (strcmp(argv[1], "version") == 0
            || strcmp(argv[1], "--version") == 0) {
        return version(argc - 1, &argv[1]);
    }

    general_usage();
    return EXIT_FAILURE;
}
