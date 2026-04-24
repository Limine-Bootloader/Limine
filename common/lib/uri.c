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
#include <compress/gzip.h>

// A URI takes the form of: resource(root):/path#hash
// The following function splits up a URI into its components.
// Note: Returns pointers into a static buffer. Callers must copy values
// if they need to persist across multiple uri_resolve() calls.
bool uri_resolve(char *uri, char **resource, char **root, char **path, char **hash) {
    #define URI_BUF_SIZE 4096
    static char buf[URI_BUF_SIZE];

    size_t length = strlen(uri) + 1;
    if (length > URI_BUF_SIZE) {
        panic(true, "uri_resolve: URI too long (max %u)", URI_BUF_SIZE - 1);
    }
    memcpy(buf, uri, length);
    uri = buf;

    *resource = *root = *path = *hash = NULL;

    // Get resource
    for (size_t i = 0; ; i++) {
        if (strlen(uri + i) < 1)
            return false;

        if (!strncmp(uri + i, "(", 1)) {
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

        if (!strncmp(uri + i, "):/", 3)) {
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

        // Validate all 128 characters are valid hexadecimal
        for (size_t j = 0; j < 128; j++) {
            char c = uri[i + j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                panic(true, "Blake2b hash contains invalid character at position %d", (int)j);
                return false;
            }
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

// Release a range of memory previously reserved with memmap_alloc_range.
// Works for both low and high addresses, unlike pmm_free which truncates
// on 32-bit builds.
static void uri_release_range(uint64_t addr, uint64_t count) {
    count = ALIGN_UP(count, 4096, panic(false, "uri: alignment overflow"));
    memmap_alloc_range(addr, count, MEMMAP_USABLE, 0, false, false, true);
}

// Allocate `count` bytes via ext_mem_alloc_type_aligned_mode and return
// the physical address in *out_addr. When allow_high_mem is true on i386
// and the allocator landed above 4 GiB, *out_low is set to NULL and the
// 64-bit address is stored in *out_addr. Otherwise *out_low points at the
// allocation and *out_addr == (uintptr_t)*out_low.
static void uri_alloc(uint64_t count, uint32_t type, bool allow_high_mem,
                      void **out_low, uint64_t *out_addr) {
    void *ret = ext_mem_alloc_type_aligned_mode(count, type, 4096, allow_high_mem);
#if defined (__i386__)
    if (allow_high_mem) {
        uint64_t addr = *(uint64_t *)ret;
        if (addr >= 0x100000000) {
            *out_low = NULL;
            *out_addr = addr;
            return;
        }
        ret = (void *)(uintptr_t)addr;
    }
#else
    (void)allow_high_mem;
#endif
    *out_low = ret;
    *out_addr = (uintptr_t)ret;
}

struct file_handle *uri_open(char *uri, uint32_t type, bool allow_high_mem
#if defined (__i386__)
    , void (*memcpy_to_64)(uint64_t dst, void *src, size_t count)
    , void (*memcpy_from_64)(void *dst, uint64_t src, size_t count)
#endif
) {
#if defined (__i386__)
    if (memcpy_to_64 == NULL || memcpy_from_64 == NULL) {
        allow_high_mem = false;
    }
#endif

    struct file_handle *raw;

    char *resource = NULL, *root = NULL, *path = NULL, *hash = NULL;
    if (!uri_resolve(uri, &resource, &root, &path, &hash)) {
        return NULL;
    }

    if (resource == NULL) {
        panic(true, "No resource specified for URI `%#`.", uri);
    }

    bool gz_compressed = *resource == '$';
    if (gz_compressed) {
        resource++;
    }

    if (!strcmp(resource, "hdd")) {
        raw = uri_hdd_dispatch(root, path);
    } else if (!strcmp(resource, "odd")) {
        raw = uri_odd_dispatch(root, path);
    } else if (!strcmp(resource, "boot")) {
        raw = uri_boot_dispatch(root, path);
    } else if (!strcmp(resource, "guid")) {
        raw = uri_guid_dispatch(root, path);
    } else if (!strcmp(resource, "uuid")) {
        raw = uri_guid_dispatch(root, path);
    } else if (!strcmp(resource, "fslabel")) {
        raw = uri_fslabel_dispatch(root, path);
    } else if (!strcmp(resource, "tftp")) {
        raw = uri_tftp_dispatch(root, path);
    } else {
        panic(true, "Resource `%s` not valid.", resource);
    }

    if (raw == NULL) {
        return NULL;
    }

    if (secure_boot_active && hash == NULL) {
        panic(true, "Secure Boot is active and URI `%#` has no associated hash!", uri);
    }

    uint8_t hash_buf[BLAKE2B_OUT_BYTES];
    if (hash != NULL) {
        for (size_t i = 0; i < sizeof(hash_buf); i++) {
            hash_buf[i] = digit_to_int(hash[i * 2]) << 4 | digit_to_int(hash[i * 2 + 1]);
        }
    }

    // Snapshot metadata from raw before the close cascade frees its buffers.
    struct volume *raw_vol = raw->vol;
    size_t raw_path_len = raw->path_len;
    char *raw_path_copy = NULL;
    if (raw->path != NULL && raw_path_len > 0) {
        raw_path_copy = ext_mem_alloc(raw_path_len);
        memcpy(raw_path_copy, raw->path, raw_path_len);
    }
#if defined (UEFI)
    EFI_HANDLE raw_efi_part = raw->efi_part_handle;
#endif
    bool raw_pxe = raw->pxe;
    uint32_t raw_pxe_ip = raw->pxe_ip;
    uint16_t raw_pxe_port = raw->pxe_port;

    // Build the filter chain: raw -> blake2b -> gzip. blake2b hashes on-disk
    // (compressed) bytes.
    struct file_handle *top = raw;
    struct file_handle *hash_fh = NULL;
    if (hash != NULL) {
        hash_fh = blake2b_open(top);
        top = hash_fh;
    }
    if (gz_compressed) {
        top = gzip_open(top);
    }

    // Drain the stream into a final allocation.
    void *buf_low = NULL;
    uint64_t buf_addr = 0;
    uint64_t buf_cap = 0;
    uint64_t buf_len = 0;
    bool is_high = false;

    if (!gz_compressed) {
        // Size is authoritative. Single up-front allocation, one copy.
        uint64_t sz = top->size;
        uri_alloc(sz, type, allow_high_mem, &buf_low, &buf_addr);
        is_high = (buf_low == NULL);

#if defined (__i386__)
        if (is_high) {
            // 1 MiB bounce loop, same as the old freadall_mode high path.
            void *pool = ext_mem_alloc(0x100000);
            for (uint64_t i = 0; i < sz; i += 0x100000) {
                size_t chunk = sz - i < 0x100000 ? (size_t)(sz - i) : 0x100000;
                uint64_t got = top->read(top, pool, i, chunk);
                if (got != chunk) {
                    panic(false, "uri: short read from non-gzip stream");
                }
                memcpy_to_64(buf_addr + i, pool, chunk);
            }
            pmm_free(pool, 0x100000);
        } else
#endif
        {
            // In-place fill.
            if (sz > 0) {
                uint64_t got = top->read(top, buf_low, 0, sz);
                if (got != sz) {
                    panic(false, "uri: short read from non-gzip stream");
                }
            }
        }
        buf_len = sz;
    } else {
        // Size is unknown (UINT64_MAX from gzip_open). Stretchy vector.
        // Initial capacity: 1 MiB, doubles on exhaustion.
        buf_cap = 0x100000;
        uri_alloc(buf_cap, type, allow_high_mem, &buf_low, &buf_addr);
        is_high = (buf_low == NULL);

#if defined (__i386__)
        // High-path uses a 1 MiB bounce pool for both the read side and
        // the grow-copy; reused across iterations.
        void *pool = is_high ? ext_mem_alloc(0x100000) : NULL;
#endif

        for (;;) {
            if (buf_len == buf_cap) {
                // Grow: double up to 64 MiB, then add 64 MiB per step.
                // Doubling past that wastes too much memory on large files.
                uint64_t new_cap = buf_cap < 0x4000000
                    ? buf_cap * 2
                    : buf_cap + 0x4000000;
                uint64_t delta = new_cap - buf_cap;

                // Try to extend in place by claiming the USABLE range
                // immediately below the current buffer. The allocator is
                // top-down, so above is already taken; below is the only
                // direction that can be contiguous. On success we only
                // pay delta extra bytes, not 2x peak.
                if (buf_addr >= delta &&
                    memmap_alloc_range(buf_addr - delta, delta, type,
                                       MEMMAP_USABLE, false, false, false)) {
                    uint64_t base = buf_addr - delta;
                    // Move existing data down. dest < src, forward-safe.
#if defined (__i386__)
                    if (is_high) {
                        for (uint64_t off = 0; off < buf_len; off += 0x100000) {
                            size_t chunk = buf_len - off < 0x100000 ? (size_t)(buf_len - off) : 0x100000;
                            memcpy_from_64(pool, buf_addr + off, chunk);
                            memcpy_to_64(base + off, pool, chunk);
                        }
                    } else
#endif
                    {
                        memmove((void *)(uintptr_t)base, buf_low, buf_len);
                        buf_low = (void *)(uintptr_t)base;
                    }
                    buf_addr = base;
                    buf_cap = new_cap;
                    goto grew;
                }

                void *new_low = NULL;
                uint64_t new_addr = 0;
                uri_alloc(new_cap, type, allow_high_mem, &new_low, &new_addr);
                bool new_is_high = (new_low == NULL);

#if defined (__i386__)
                if (is_high && new_is_high) {
                    // 64-to-64: bounce via low pool in 1 MiB strides.
                    for (uint64_t off = 0; off < buf_len; off += 0x100000) {
                        size_t chunk = buf_len - off < 0x100000 ? (size_t)(buf_len - off) : 0x100000;
                        memcpy_from_64(pool, buf_addr + off, chunk);
                        memcpy_to_64(new_addr + off, pool, chunk);
                    }
                } else if (is_high && !new_is_high) {
                    // Shouldn't happen: once we landed high we ask for high.
                    // Keep a defensive path: bounce via pool, then memcpy.
                    for (uint64_t off = 0; off < buf_len; off += 0x100000) {
                        size_t chunk = buf_len - off < 0x100000 ? (size_t)(buf_len - off) : 0x100000;
                        memcpy_from_64(pool, buf_addr + off, chunk);
                        memcpy((uint8_t *)new_low + off, pool, chunk);
                    }
                } else if (!is_high && new_is_high) {
                    for (uint64_t off = 0; off < buf_len; off += 0x100000) {
                        size_t chunk = buf_len - off < 0x100000 ? (size_t)(buf_len - off) : 0x100000;
                        memcpy_to_64(new_addr + off, (uint8_t *)buf_low + off, chunk);
                    }
                } else
#endif
                {
                    (void)new_is_high;   /*  Silence unused warning on non-i386.  */
                    memcpy(new_low, buf_low, buf_len);
                }

                // Release the old allocation.
                uri_release_range(buf_addr, buf_cap);

                buf_low = new_low;
                buf_addr = new_addr;
                buf_cap = new_cap;
#if defined (__i386__)
                if (is_high != new_is_high && new_is_high && pool == NULL) {
                    pool = ext_mem_alloc(0x100000);
                }
                is_high = new_is_high;
#endif
grew:;
            }

            uint64_t want = buf_cap - buf_len;
            if (want > 65536) want = 65536;

            uint64_t got;
#if defined (__i386__)
            if (is_high) {
                got = top->read(top, pool, buf_len, want);
                if (got > 0) memcpy_to_64(buf_addr + buf_len, pool, got);
            } else
#endif
            {
                got = top->read(top, (uint8_t *)buf_low + buf_len, buf_len, want);
            }
            if (got == 0) break;
            buf_len += got;
        }

        // Release the page-aligned tail past the actual data so we don't
        // hand the OS up to 64 MiB of slack typed as `type`. Keep at least
        // one page so the returned handle has a valid address.
        uint64_t kept = ALIGN_UP(buf_len, 4096, panic(true, "uri: alignment overflow"));
        if (kept == 0) kept = 4096;
        if (kept < buf_cap) {
            uri_release_range(buf_addr + kept, buf_cap - kept);
            buf_cap = kept;
        }

#if defined (__i386__)
        if (pool != NULL) pmm_free(pool, 0x100000);
#endif
    }

    // Finalize hash check now that all compressed bytes have flowed through
    // the filter.
    if (hash_fh != NULL) {
        if (!blake2b_check_hash(hash_fh, hash_buf)) {
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

    // Close the filter chain. fclose cascades.
    fclose(top);

    // Build the returned memfile. Fresh allocation so we never mutate any
    // closed filter handle's state.
    struct file_handle *out = ext_mem_alloc(sizeof(struct file_handle));
    out->is_memfile = true;
    out->readall = true;
    out->is_high_mem = is_high;
    out->fd = is_high ? NULL : buf_low;
    out->load_addr_64 = buf_addr;
    out->size = buf_len;
    out->vol = raw_vol;
    out->path = raw_path_copy;
    out->path_len = raw_path_copy != NULL ? raw_path_len : 0;
#if defined (UEFI)
    out->efi_part_handle = raw_efi_part;
#endif
    out->pxe = raw_pxe;
    out->pxe_ip = raw_pxe_ip;
    out->pxe_port = raw_pxe_port;

    return out;
}
