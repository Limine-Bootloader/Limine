#include <stdint.h>
#include <stddef.h>
#include <lib/uri.h>
#include <lib/misc.h>
#include <lib/part.h>
#include <lib/libc.h>
#include <lib/config.h>
#include <fs/file.h>
#include <mm/pmm.h>
#include <lib/print.h>
#include <pxe/tftp.h>
#include <menu.h>
#include <lib/getchar.h>
#include <crypt/blake2b.h>

// A URI takes the form of: resource(root):/path#hash
// The following function splits up a URI into its components
bool uri_resolve(char *uri, char **resource, char **root, char **path, char **hash) {
    size_t length = strlen(uri) + 1;
    char *buf = ext_mem_alloc(length);
    memcpy(buf, uri, length);
    uri = buf;

    *resource = *root = *path = NULL;

    // Get resource
    for (size_t i = 0; ; i++) {
        if (strlen(uri + i) < 1)
            return false;

        if (!memcmp(uri + i, "(", 1)) {
            *resource = uri;
            uri[i] = 0;
            uri += i + 1;
            break;
        }
    }

    // Get root
    for (size_t i = 0; ; i++) {
        if (strlen(uri + i) < 3)
            return false;

        if (!memcmp(uri + i, "):/", 3)) {
            *root = uri;
            uri[i] = 0;
            uri += i + 3;
            break;
        }
    }

    // Get path
    if (*uri == 0)
        return false;
    *path = uri;

    // Get hash
    for (int i = (int)strlen(uri) - 1; i >= 0; i--) {
        if (uri[i] != '#') {
            continue;
        }

        uri[i++] = 0;

        if (hash != NULL) {
            *hash = uri + i;
        }

        if (strlen(uri + i) != 128) {
            panic(true, "Blake2b hash must be 128 characters long");
            return false;
        }

        break;
    }

    return true;
}

static bool parse_bios_partition(char *loc, int *drive, int *partition) {
    uint64_t val;

    for (size_t i = 0; ; i++) {
        if (loc[i] == 0)
            return false;

        if (loc[i] == ':') {
            loc[i] = 0;
            if (*loc == 0) {
                panic(true, "Drive number cannot be omitted for hdd():/ and odd():/");
            } else {
                val = strtoui(loc, NULL, 10);
                if (val < 1 || val > 256) {
                    panic(true, "Drive number outside range 1-256");
                }
                *drive = val;
            }
            loc += i + 1;
            break;
        }
    }

    val = strtoui(loc, NULL, 10);
    if (val > 256) {
        panic(true, "Partition number outside range 0-256");
    }
    *partition = val;

    return true;
}

static struct file_handle *uri_hdd_dispatch(char *loc, char *path) {
    int drive, partition;

    if (!parse_bios_partition(loc, &drive, &partition))
        return NULL;

    struct volume *volume = volume_get_by_coord(false, drive, partition);

    if (volume == NULL)
        return NULL;

    return fopen(volume, path);
}

static struct file_handle *uri_odd_dispatch(char *loc, char *path) {
    int drive, partition;

    if (!parse_bios_partition(loc, &drive, &partition))
        return NULL;

    struct volume *volume = volume_get_by_coord(true, drive, partition);

    if (volume == NULL)
        return NULL;

    return fopen(volume, path);
}

static struct file_handle *uri_guid_dispatch(char *guid_str, char *path) {
    struct guid guid;
    if (!string_to_guid_be(&guid, guid_str))
        return NULL;

    struct volume *volume = volume_get_by_guid(&guid);
    if (volume == NULL) {
        if (!string_to_guid_mixed(&guid, guid_str))
            return NULL;

        volume = volume_get_by_guid(&guid);
        if (volume == NULL)
            return NULL;
    }

    return fopen(volume, path);
}

static struct file_handle *uri_fslabel_dispatch(char *fslabel, char *path) {
    struct volume *volume = volume_get_by_fslabel(fslabel);
    if (volume == NULL) {
        return NULL;
    }

    return fopen(volume, path);
}

static struct file_handle *uri_tftp_dispatch(char *root, char *path) {
    uint32_t ip;
    if (!strcmp(root, "")) {
        ip = 0;
    } else {
        if (inet_pton(root, &ip)) {
            panic(true, "tftp: Invalid ipv4 address: %s", root);
        }
    }

    struct file_handle *ret;
    if ((ret = tftp_open(boot_volume, root, path)) == NULL) {
        return NULL;
    }

    return ret;
}

static struct file_handle *uri_boot_dispatch(char *s_part, char *path) {
    if (boot_volume->pxe)
        return uri_tftp_dispatch(s_part, path);

    int partition;

    if (s_part[0] != '\0') {
        uint64_t val = strtoui(s_part, NULL, 10);
        if (val > 256) {
            panic(true, "Partition number outside range 0-256");
        }
        partition = val;
    } else {
        partition = boot_volume->partition;
    }

    struct volume *volume = volume_get_by_coord(boot_volume->is_optical,
                                                boot_volume->index, partition);
    if (volume == NULL)
        return NULL;

    return fopen(volume, path);
}

struct file_handle *uri_open(char *uri) {
    struct file_handle *ret;

    char *resource = NULL, *root = NULL, *path = NULL, *hash = NULL;
    if (!uri_resolve(uri, &resource, &root, &path, &hash)) {
        return NULL;
    }

    if (resource == NULL) {
        panic(true, "No resource specified for URI `%#`.", uri);
    }

    if (!strcmp(resource, "hdd")) {
        ret = uri_hdd_dispatch(root, path);
    } else if (!strcmp(resource, "odd")) {
        ret = uri_odd_dispatch(root, path);
    } else if (!strcmp(resource, "boot")) {
        ret = uri_boot_dispatch(root, path);
    } else if (!strcmp(resource, "guid")) {
        ret = uri_guid_dispatch(root, path);
    } else if (!strcmp(resource, "uuid")) {
        ret = uri_guid_dispatch(root, path);
    } else if (!strcmp(resource, "fslabel")) {
        ret = uri_fslabel_dispatch(root, path);
    } else if (!strcmp(resource, "tftp")) {
        ret = uri_tftp_dispatch(root, path);
    } else {
        panic(true, "Resource `%s` not valid.", resource);
    }

    if (hash != NULL && ret != NULL) {
        uint8_t out_buf[BLAKE2B_OUT_BYTES];
#if defined (UEFI) && defined (__x86_64__)
        void *file_buf = freadall_mode(ret, MEMMAP_BOOTLOADER_RECLAIMABLE, true);
#else
        void *file_buf = freadall(ret, MEMMAP_BOOTLOADER_RECLAIMABLE);
#endif
        blake2b(out_buf, file_buf, ret->size);
        uint8_t hash_buf[BLAKE2B_OUT_BYTES];

        for (size_t i = 0; i < sizeof(hash_buf); i++) {
            hash_buf[i] = digit_to_int(hash[i * 2]) << 4 | digit_to_int(hash[i * 2 + 1]);
        }

        if (memcmp(hash_buf, out_buf, sizeof(out_buf)) != 0) {
            if (hash_mismatch_panic) {
                panic(true, "Blake2b hash for URI `%#` does not match!", uri);
            } else {
                print("WARNING: Blake2b hash for URI `%#` does not match!\n"
                      "         Press Y to continue, press any other key to return to menu...", uri);

                char ch = getchar();
                if (ch != 'Y' && ch != 'y') {
                    menu(false);
                }
                print("\n");
            }
        }
    }

    return ret;
}
