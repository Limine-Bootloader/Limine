#undef IS_WINDOWS
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)
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
#include <time.h>

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

// This table from https://web.mit.edu/freebsd/head/sys/libkern/crc32.c
static const uint32_t crc32_table[] = {
	0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
	0xe963a535, 0x9e6495a3,	0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
	0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
	0xf3b97148, 0x84be41de,	0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
	0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,	0x14015c4f, 0x63066cd9,
	0xfa0f3d63, 0x8d080df5,	0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
	0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,	0x35b5a8fa, 0x42b2986c,
	0xdbbbc9d6, 0xacbcf940,	0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
	0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
	0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
	0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,	0x76dc4190, 0x01db7106,
	0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
	0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
	0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
	0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
	0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
	0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
	0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
	0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
	0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
	0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
	0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
	0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
	0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
	0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
	0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
	0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
	0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
	0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
	0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
	0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
	0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
	0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
	0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
	0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
	0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
	0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
	0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
	0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
	0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
	0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
	0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
	0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
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

static uint32_t crc32(void *_stream, size_t len) {
    uint8_t *stream = _stream;
    uint32_t ret = 0xffffffff;

    for (size_t i = 0; i < len; i++) {
        ret = (ret >> 8) ^ crc32_table[(ret ^ stream[i]) & 0xff];
    }

    ret ^= 0xffffffff;
    return ret;
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

    for (size_t i = 0; i < sizeof(guesses) / sizeof(size_t); i++) {
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

    fprintf(stderr, "%s: error: device_init(): Couldn't determine block size of device.\n", program_name);
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

    fclose(udfile);
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
    if (!quiet) {
        fprintf(stderr, "Loading uninstall data from file: `%s`...\n", filename);
    }

    FILE *udfile = fopen(filename, "rb");
    if (udfile == NULL) {
        perror_wrap("error: `%s`", filename);
        goto error;
    }

    if (fread(&uninstall_data_i, sizeof(uint64_t), 1, udfile) != 1) {
        goto fread_error;
    }

    for (size_t i = 0; i < uninstall_data_i; i++) {
        if (fread(&uninstall_data[i].loc, sizeof(uint64_t), 1, udfile) != 1) {
            goto fread_error;
        }
        if (fread(&uninstall_data[i].count, sizeof(uint64_t), 1, udfile) != 1) {
            goto fread_error;
        }
        uninstall_data[i].data = malloc(uninstall_data[i].count);
        if (uninstall_data[i].data == NULL) {
            perror_wrap("error: load_uninstall_data(): malloc()");
            goto error;
        }
        if (fread(uninstall_data[i].data, uninstall_data[i].count, 1, udfile) != 1) {
            goto fread_error;
        }
    }

    fclose(udfile);
    return true;

fread_error:
    perror_wrap("error: load_uninstall_data(): fread()");

error:
    if (udfile != NULL) {
        fclose(udfile);
    }
    return false;
}

static bool _device_read(void *_buffer, uint64_t loc, size_t count) {
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

static bool _device_write(const void *_buffer, uint64_t loc, size_t count) {
    if (uninstalling) {
        goto skip_save;
    }

    if (uninstall_data_i >= UNINSTALL_DATA_MAX) {
        fprintf(stderr, "%s: error: Too many uninstall data entries! Please report this bug upstream.\n", program_name);
        return false;
    }

    struct uninstall_data *ud = &uninstall_data[uninstall_data_i];

    ud->data = malloc(count);
    if (ud->data == NULL) {
        perror_wrap("error: _device_write(): malloc()");
        return false;
    }

    if (!_device_read(ud->data, loc, count)) {
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
        while (!_device_write(ud->data, ud->loc, ud->count)) {
            if (retry) {
                fprintf(stderr, "%s: warning: Retry failed.\n", program_name);
                print_write_fail = true;
                break;
            }
            if (!quiet) {
                fprintf(stderr, "%s: warning: Uninstall data index %zu failed to write, retrying...\n", program_name, i);
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
        fprintf(stderr, "%s: error: Some data failed to be uninstalled correctly.\n", program_name);
        ret = false;
    }

    if (print_cache_flush_fail) {
        fprintf(stderr, "%s: error: Device cache flush failure. Uninstall may be incomplete.\n", program_name);
        ret = false;
    }

    if (ret == true && !quiet && !quiet_arg) {
        fprintf(stderr, "Uninstall data restored successfully.\n");
    }

    return ret;
}

#define device_read(BUFFER, LOC, COUNT)        \
    do {                                       \
        if (!_device_read(BUFFER, LOC, COUNT)) \
            goto cleanup;                      \
    } while (0)

#define device_write(BUFFER, LOC, COUNT)        \
    do {                                        \
        if (!_device_write(BUFFER, LOC, COUNT)) \
            goto cleanup;                       \
    } while (0)

static void bios_install_usage(void) {
    printf("usage: %s bios-install <device> [GPT partition index]\n", program_name);
    printf("\n");
    printf("    --force-mbr     Force MBR detection to work even if the\n");
    printf("                    safety checks fail (DANGEROUS!)\n");
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

static int bios_install(int argc, char *argv[]) {
    int      ok = EXIT_FAILURE;
    int      force_mbr = 0;
    bool gpt2mbr_allowed = true;
    bool uninstall_mode = false;
    const uint8_t *bootloader_img = binary_limine_hdd_bin_data;
    size_t   bootloader_file_size = sizeof(binary_limine_hdd_bin_data);
    uint8_t  orig_mbr[70], timestamp[6];
    const char *part_ndx = NULL;

#ifndef __BYTE_ORDER__
    uint32_t endcheck = 0x12345678;
    uint8_t endbyte = *((uint8_t *)&endcheck);
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
        } else if (strcmp(argv[i], "--force-mbr") == 0) {
            if (force_mbr && !quiet) {
                fprintf(stderr, "%s: warning: --force-mbr already set.\n", program_name);
            }
            force_mbr = 1;
        } else if (strcmp(argv[i], "--no-gpt-to-mbr-isohybrid-conversion") == 0) {
            gpt2mbr_allowed = false;
        } else if (strcmp(argv[i], "--uninstall") == 0) {
            if (uninstall_mode && !quiet) {
                fprintf(stderr, "%s: warning: --uninstall already set.\n", program_name);
            }
            uninstall_mode = true;
        } else if (memcmp(argv[i], "--uninstall-data-file=", 21) == 0) {
            if (uninstall_file != NULL && !quiet) {
                fprintf(stderr, "%s: warning: --uninstall-data-file already set. Overriding...\n", program_name);
            }
            uninstall_file = argv[i] + 21;
            if (strlen(uninstall_file) == 0) {
                fprintf(stderr, "%s: error: Uninstall data file has a zero-length name!\n", program_name);
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
        fprintf(stderr, "%s: error: No device specified\n", program_name);
        bios_install_usage();
        return EXIT_FAILURE;
    }

    if (!device_init()) {
        goto uninstall_mode_cleanup;
    }

    if (uninstall_mode) {
        if (uninstall_file == NULL) {
            fprintf(stderr, "%s: error: Uninstall mode set but no --uninstall-data-file=... passed.\n", program_name);
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
    uint64_t lb_guesses[] = { 512, 4096 };
    uint64_t lb_size = 0;
    for (size_t i = 0; i < sizeof(lb_guesses) / sizeof(uint64_t); i++) {
        device_read(&gpt_header, lb_guesses[i], sizeof(struct gpt_table_header));
        if (!strncmp(gpt_header.signature, "EFI PART", 8)) {
            lb_size = lb_guesses[i];
            if (!force_mbr) {
                gpt = 1;
                if (!quiet) {
                    fprintf(stderr, "Installing to GPT. Logical block size of %" PRIu64 " bytes.\n",
                            lb_guesses[i]);
                }
            } else {
                fprintf(stderr, "%s: error: Device has a valid GPT, refusing to force MBR.\n", program_name);
                goto cleanup;
            }
            break;
        }
    }

    struct gpt_table_header secondary_gpt_header;
    if (gpt) {
        if (!quiet) {
            fprintf(stderr, "Secondary header at LBA 0x%" PRIx64 ".\n",
                    ENDSWAP(gpt_header.alternate_lba));
        }
        device_read(&secondary_gpt_header, lb_size * ENDSWAP(gpt_header.alternate_lba),
              sizeof(struct gpt_table_header));
        if (!strncmp(secondary_gpt_header.signature, "EFI PART", 8)) {
            if (!quiet) {
                fprintf(stderr, "Secondary header valid.\n");
            }
        } else {
            fprintf(stderr, "%s: error: Secondary header not valid, aborting.\n", program_name);
            goto cleanup;
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

        for (int64_t i = 0; i < (int64_t)ENDSWAP(gpt_header.number_of_partition_entries); i++) {
            struct gpt_entry gpt_entry;
            device_read(&gpt_entry,
                        (ENDSWAP(gpt_header.partition_entry_lba) * lb_size)
                        + (i * ENDSWAP(gpt_header.size_of_partition_entry)),
                        sizeof(struct gpt_entry));

            if (gpt_entry.unique_partition_guid[0] == 0 &&
                gpt_entry.unique_partition_guid[1] == 0) {
                continue;
            }

            if (ENDSWAP(gpt_entry.starting_lba) > UINT32_MAX) {
                if (!quiet) {
                    fprintf(stderr, "Starting LBA of partition %zu is greater than UINT32_MAX, will not convert GPT.\n", i + 1);
                }
                goto no_mbr_conv;
            }
            part_to_conv[part_to_conv_i].lba_start = ENDSWAP(gpt_entry.starting_lba);
            lba2chs(part_to_conv[part_to_conv_i].chs_start, part_to_conv[part_to_conv_i].lba_start);

            if (ENDSWAP(gpt_entry.ending_lba) > UINT32_MAX) {
                if (!quiet) {
                    fprintf(stderr, "Ending LBA of partition %zu is greater than UINT32_MAX, will not convert GPT.\n", i + 1);
                }
                goto no_mbr_conv;
            }
            part_to_conv[part_to_conv_i].lba_end = ENDSWAP(gpt_entry.ending_lba);
            lba2chs(part_to_conv[part_to_conv_i].chs_end, part_to_conv[part_to_conv_i].lba_end);

            int type = gpt2mbr_type(ENDSWAP(gpt_entry.partition_type_guid[0]),
                                    ENDSWAP(gpt_entry.partition_type_guid[1]));
            if (type == -1) {
                if (!quiet) {
                    fprintf(stderr, "Cannot convert partition type for partition %zu, will not convert GPT.\n", i + 1);
                }
                goto no_mbr_conv;
            }

            if (part_to_conv_i == 4) {
                if (!quiet) {
                    fprintf(stderr, "GPT contains more than 4 partitions, will not convert.\n");
                }
                goto no_mbr_conv;
            }

            part_to_conv[part_to_conv_i].type = type;

            part_to_conv_i++;
        }

        // Nuke the GPTs.
        void *empty_lba = calloc(1, lb_size);
        if (empty_lba == NULL) {
            perror_wrap("error: bios_install(): malloc()");
            goto cleanup;
        }

        // ... nuke primary GPT + protective MBR.
        for (size_t i = 0; i < 34; i++) {
            device_write(empty_lba, i * lb_size, lb_size);
        }

        // ... nuke secondary GPT.
        for (size_t i = 0; i < 33; i++) {
            device_write(empty_lba, ((ENDSWAP(gpt_header.alternate_lba) - 32) + i) * lb_size, lb_size);
        }

        free(empty_lba);

        // We're no longer GPT.
        gpt = 0;

        // Generate pseudorandom MBR disk ID.
        srand(time(NULL));
        for (size_t i = 0; i < 4; i++) {
            uint8_t r = rand();
            device_write(&r, 0x1b8 + i, 1);
        }

        // Write out the partition entries.
        for (size_t i = 0; i < part_to_conv_i; i++) {
            device_write(&part_to_conv[i].type, 0x1be + i * 16 + 0x04, 1);
            uint32_t lba_start = ENDSWAP(part_to_conv[i].lba_start);
            device_write(&lba_start, 0x1be + i * 16 + 0x08, 4);
            uint32_t sect_count = ENDSWAP((part_to_conv[i].lba_end - part_to_conv[i].lba_start) + 1);
            device_write(&sect_count, 0x1be + i * 16 + 0x0c, 4);

            device_write(part_to_conv[i].chs_start, 0x1be + i * 16 + 1, 3);
            device_write(part_to_conv[i].chs_end, 0x1be + i * 16 + 5, 3);
        }

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

        device_read(&hint8, 446, sizeof(uint8_t));
        if (hint8 != 0x00 && hint8 != 0x80) {
            if (!force_mbr) {
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
            if (!force_mbr) {
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
            if (!force_mbr) {
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
            if (!force_mbr) {
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
            fprintf(stderr, "%s: error: A partition's start sector is less than 63, aborting.\n", program_name);
            goto cleanup;
        }

        char hintc[64];
        device_read(hintc, 4, 8);
        if (memcmp(hintc, "_ECH_FS_", 8) == 0) {
            if (!force_mbr) {
                mbr = 0;
            } else {
                memset(hintc, 0, 8);
                device_write(hintc, 4, 8);
            }
        }
        device_read(hintc, 3, 4);
        if (memcmp(hintc, "NTFS", 4) == 0) {
            if (!force_mbr) {
                mbr = 0;
            } else {
                memset(hintc, 0, 4);
                device_write(hintc, 3, 4);
            }
        }
        device_read(hintc, 54, 3);
        if (memcmp(hintc, "FAT", 3) == 0) {
            if (!force_mbr) {
                mbr = 0;
            } else {
                memset(hintc, 0, 5);
                device_write(hintc, 54, 5);
            }
        }
        device_read(hintc, 82, 3);
        if (memcmp(hintc, "FAT", 3) == 0) {
            if (!force_mbr) {
                mbr = 0;
            } else {
                memset(hintc, 0, 5);
                device_write(hintc, 82, 5);
            }
        }
        device_read(hintc, 3, 5);
        if (memcmp(hintc, "FAT32", 5) == 0) {
            if (!force_mbr) {
                mbr = 0;
            } else {
                memset(hintc, 0, 5);
                device_write(hintc, 3, 5);
            }
        }
        device_read(&hint16, 1080, sizeof(uint16_t));
        hint16 = ENDSWAP(hint16);
        if (hint16 == 0xef53) {
            if (!force_mbr) {
                mbr = 0;
            } else {
                hint16 = 0;
                hint16 = ENDSWAP(hint16);
                device_write(&hint16, 1080, sizeof(uint16_t));
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
        fprintf(stderr, "       Alternatively, pass `--force-mbr` to override these checks.\n");
        fprintf(stderr, "       **ONLY DO THIS AT YOUR OWN RISK, DATA LOSS MAY OCCUR!**\n");
        goto cleanup;
    }

    size_t   stage2_size   = bootloader_file_size - 512;

    size_t   stage2_sects  = DIV_ROUNDUP(stage2_size, 512);

    uint16_t stage2_size_a = (stage2_sects / 2) * 512 + (stage2_sects % 2 ? 512 : 0);
    uint16_t stage2_size_b = (stage2_sects / 2) * 512;

    // Default split of stage2 for MBR (consecutive in post MBR gap)
    uint64_t stage2_loc_a = 512;
    uint64_t stage2_loc_b = stage2_loc_a + stage2_size_a;

    if (gpt) {
        if (part_ndx != NULL) {
            uint32_t partition_num;
            sscanf(part_ndx, "%" SCNu32, &partition_num);
            partition_num--;
            if (partition_num > ENDSWAP(gpt_header.number_of_partition_entries)) {
                fprintf(stderr, "%s: error: Partition number is too large.\n", program_name);
                goto cleanup;
            }

            struct gpt_entry gpt_entry;
            device_read(&gpt_entry,
                (ENDSWAP(gpt_header.partition_entry_lba) * lb_size)
                + (partition_num * ENDSWAP(gpt_header.size_of_partition_entry)),
                sizeof(struct gpt_entry));

            if (gpt_entry.unique_partition_guid[0] == 0 &&
              gpt_entry.unique_partition_guid[1] == 0) {
                fprintf(stderr, "%s: error: No such partition: `%s`.\n", program_name, part_ndx);
                goto cleanup;
            }

            if (!quiet) {
                fprintf(stderr, "GPT partition specified. Installing there instead of embedding.\n");
            }

            stage2_loc_a = ENDSWAP(gpt_entry.starting_lba) * lb_size;
            stage2_loc_b = stage2_loc_a + stage2_size_a;
            if (stage2_loc_b & (lb_size - 1))
                stage2_loc_b = (stage2_loc_b + lb_size) & ~(lb_size - 1);
        } else {
            if (!quiet) {
                fprintf(stderr, "GPT partition NOT specified. Attempting GPT embedding.\n");
            }

            int64_t max_partition_entry_used = -1;
            for (int64_t i = 0; i < (int64_t)ENDSWAP(gpt_header.number_of_partition_entries); i++) {
                struct gpt_entry gpt_entry;
                device_read(&gpt_entry,
                    (ENDSWAP(gpt_header.partition_entry_lba) * lb_size)
                      + (i * ENDSWAP(gpt_header.size_of_partition_entry)),
                    sizeof(struct gpt_entry));

                if (gpt_entry.unique_partition_guid[0] != 0 ||
                  gpt_entry.unique_partition_guid[1] != 0) {
                    if (i > max_partition_entry_used)
                        max_partition_entry_used = i;
                }
            }

            stage2_loc_a  = (ENDSWAP(gpt_header.partition_entry_lba) + 32) * lb_size;
            stage2_loc_a -= stage2_size_a;
            stage2_loc_a &= ~(lb_size - 1);
            stage2_loc_b  = (ENDSWAP(secondary_gpt_header.partition_entry_lba) + 32) * lb_size;
            stage2_loc_b -= stage2_size_b;
            stage2_loc_b &= ~(lb_size - 1);

            size_t partition_entries_per_lb =
                lb_size / ENDSWAP(gpt_header.size_of_partition_entry);
            size_t new_partition_array_lba_size =
                stage2_loc_a / lb_size - ENDSWAP(gpt_header.partition_entry_lba);
            size_t new_partition_entry_count =
                new_partition_array_lba_size * partition_entries_per_lb;

            if ((int64_t)new_partition_entry_count <= max_partition_entry_used) {
                fprintf(stderr, "%s: error: Cannot embed because there are too many used partition entries.\n", program_name);
                goto cleanup;
            }

            if (!quiet) {
                fprintf(stderr, "New maximum count of partition entries: %zu.\n", new_partition_entry_count);
            }

            // Zero out unused partitions
            void *empty = calloc(1, ENDSWAP(gpt_header.size_of_partition_entry));
            for (size_t i = max_partition_entry_used + 1; i < new_partition_entry_count; i++) {
                device_write(empty,
                    ENDSWAP(gpt_header.partition_entry_lba) * lb_size + i * ENDSWAP(gpt_header.size_of_partition_entry),
                    ENDSWAP(gpt_header.size_of_partition_entry));
            }
            for (size_t i = max_partition_entry_used + 1; i < new_partition_entry_count; i++) {
                device_write(empty,
                    ENDSWAP(secondary_gpt_header.partition_entry_lba) * lb_size + i * ENDSWAP(secondary_gpt_header.size_of_partition_entry),
                    ENDSWAP(secondary_gpt_header.size_of_partition_entry));
            }
            free(empty);

            uint8_t *partition_array =
                malloc(new_partition_entry_count * ENDSWAP(gpt_header.size_of_partition_entry));
            if (partition_array == NULL) {
                perror_wrap("error: bios_install(): malloc()");
                goto cleanup;
            }

            device_read(partition_array,
                  ENDSWAP(gpt_header.partition_entry_lba) * lb_size,
                  new_partition_entry_count * ENDSWAP(gpt_header.size_of_partition_entry));

            uint32_t crc32_partition_array =
                crc32(partition_array,
                      new_partition_entry_count * ENDSWAP(gpt_header.size_of_partition_entry));

            free(partition_array);

            gpt_header.partition_entry_array_crc32 = ENDSWAP(crc32_partition_array);
            gpt_header.number_of_partition_entries = ENDSWAP(new_partition_entry_count);
            gpt_header.crc32 = 0;
            gpt_header.crc32 = crc32(&gpt_header, 92);
            gpt_header.crc32 = ENDSWAP(gpt_header.crc32);
            device_write(&gpt_header,
                         lb_size,
                         sizeof(struct gpt_table_header));

            secondary_gpt_header.partition_entry_array_crc32 = ENDSWAP(crc32_partition_array);
            secondary_gpt_header.number_of_partition_entries =
                ENDSWAP(new_partition_entry_count);
            secondary_gpt_header.crc32 = 0;
            secondary_gpt_header.crc32 = crc32(&secondary_gpt_header, 92);
            secondary_gpt_header.crc32 = ENDSWAP(secondary_gpt_header.crc32);
            device_write(&secondary_gpt_header,
                         lb_size * ENDSWAP(gpt_header.alternate_lba),
                         sizeof(struct gpt_table_header));
        }
    } else {
        if (!quiet) {
            fprintf(stderr, "Installing to MBR.\n");
        }
    }

    if (!quiet) {
        fprintf(stderr, "Stage 2 to be located at 0x%" PRIx64 " and 0x%" PRIx64 ".\n",
                stage2_loc_a, stage2_loc_b);
    }

    // Save original timestamp
    device_read(timestamp, 218, 6);

    // Save the original partition table of the device
    device_read(orig_mbr, 440, 70);

    // Write the bootsector from the bootloader to the device
    device_write(&bootloader_img[0], 0, 512);

    // Write the rest of stage 2 to the device
    device_write(&bootloader_img[512], stage2_loc_a, stage2_size_a);
    device_write(&bootloader_img[512 + stage2_size_a],
                 stage2_loc_b, stage2_size - stage2_size_a);

    // Hardcode in the bootsector the location of stage 2 halves
    stage2_size_a = ENDSWAP(stage2_size_a);
    device_write(&stage2_size_a, 0x1a4 + 0,  sizeof(uint16_t));
    stage2_size_b = ENDSWAP(stage2_size_b);
    device_write(&stage2_size_b, 0x1a4 + 2,  sizeof(uint16_t));
    stage2_loc_a = ENDSWAP(stage2_loc_a);
    device_write(&stage2_loc_a,  0x1a4 + 4,  sizeof(uint64_t));
    stage2_loc_b = ENDSWAP(stage2_loc_b);
    device_write(&stage2_loc_b,  0x1a4 + 12, sizeof(uint64_t));

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

        fprintf(stderr, "Limine BIOS stages installed successfully!\n");
    }

    ok = EXIT_SUCCESS;

cleanup:
    reverse_uninstall_data();
    if (ok != EXIT_SUCCESS) {
        // If we failed, attempt to reverse install process
        fprintf(stderr, "%s: Install failed, undoing work...\n", program_name);
        uninstall(true);
    } else if (uninstall_file != NULL) {
        store_uninstall_data(uninstall_file);
    }
uninstall_mode_cleanup:
    free_uninstall_data();
    if (cache)
        free(cache);
    if (device != NULL)
        fclose(device);

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
    bool quiet = false;
    bool reset = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            enroll_config_usage();
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            remove_arg(&argc, argv, i);
            quiet = true;
        } else if (strcmp(argv[i], "--reset") == 0) {
            remove_arg(&argc, argv, i);
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

    if (!reset && strlen(argv[2]) != 128) {
        fprintf(stderr, "%s: error: BLAKE2B specified is not 128 characters long.\n", program_name);
        goto cleanup;
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
    size_t bootloader_size = ftell(bootloader_file);
    rewind(bootloader_file);

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
    for (size_t i = 0; i < bootloader_size - ((sizeof(CONFIG_B2SUM_SIGNATURE) - 1) + 128) + 1; i++) {
        if (bootloader[i] != config_b2sum_sign[checked_count]) {
            checked_count = 0;
            continue;
        }

        checked_count++;

        if (checked_count == sizeof(CONFIG_B2SUM_SIGNATURE) - 1) {
            checksum_loc = &bootloader[i + 1];
            break;
        }
    }

    if (checksum_loc == NULL) {
        fprintf(stderr, "%s: error: Checksum location not found in provided executable.\n", program_name);
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

    if (!quiet) {
        fprintf(stderr, "Config file BLAKE2B successfully %s!\n", reset ? "reset" : "enrolled");
    }
    ret = EXIT_SUCCESS;

cleanup:
    if (bootloader != NULL) {
        free(bootloader);
    }
    if (bootloader_file != NULL) {
        fclose(bootloader_file);
    }
    return ret;
}

#define LIMINE_VERSION "9.2.3"
#define LIMINE_COPYRIGHT "Copyright (C) 2019-2025 Mintsuki and contributors."

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
        if (strcmp(argv[1], "--help") == 0) {
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
    fprintf(stderr, "%s: error: Cannot print datadir for `limine` built out-of-tree.\n", program_name);
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
        fprintf(stderr, "%s: error: Limine has been compiled without BIOS support.\n", program_name);
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
