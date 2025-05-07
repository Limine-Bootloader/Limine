#include <stddef.h>
#include <stdint.h>
#include <fs/file.h>
#include <fs/fat32.h>
#include <fs/iso9660.h>
#include <lib/print.h>
#include <lib/misc.h>
#include <mm/pmm.h>
#include <lib/part.h>
#include <lib/libc.h>
#include <pxe/tftp.h>

char *fs_get_label(struct volume *part) {
    char *ret;

    if ((ret = fat32_get_label(part)) != NULL) {
        return ret;
    }

    return NULL;
}

bool fs_get_guid(struct guid *guid, struct volume *part) {
    (void)guid; (void)part;

    return false;
}

bool case_insensitive_fopen = false;

struct file_handle *fopen(struct volume *part, const char *filename) {
    size_t filename_new_len = strlen(filename) + 2;
    char *filename_new = ext_mem_alloc(filename_new_len);

    if (filename[0] != '/') {
        filename_new[0] = '/';
        strcpy(&filename_new[1], filename);
    } else {
        strcpy(filename_new, filename);
    }

    filename = filename_new;

    struct file_handle *ret;

    if (part->pxe) {
        if ((ret = tftp_open(part, "", filename)) == NULL) {
            return NULL;
        }
        return ret;
    }

    if ((ret = iso9660_open(part, filename)) != NULL) {
        goto success;
    }
    if ((ret = fat32_open(part, filename)) != NULL) {
        goto success;
    }

    return NULL;

success:
    ret->path = (char *)filename;
    ret->path_len = filename_new_len;

    return ret;
}

void fclose(struct file_handle *fd) {
    if (fd->is_memfile) {
        if (fd->readall == false) {
            pmm_free(fd->fd, fd->size);
        }
    } else {
        fd->close(fd);
    }
    pmm_free(fd->path, fd->path_len);
    pmm_free(fd, sizeof(struct file_handle));
}

void fread(struct file_handle *fd, void *buf, uint64_t loc, uint64_t count) {
    if (fd->is_memfile) {
        memcpy(buf, fd->fd + loc, count);
    } else {
        fd->read(fd, buf, loc, count);
    }
}

void *freadall(struct file_handle *fd, uint32_t type) {
    return freadall_mode(fd, type, false
#if defined (__i386__)
        , NULL
#endif
    );
}

void *freadall_mode(struct file_handle *fd, uint32_t type, bool allow_high_allocs
#if defined (__i386__)
    , void (*memcpy_to_64)(uint64_t dst, void *src, size_t count)
#endif
) {
#if defined (__i386__)
    static uint64_t high_ret;

    if (memcpy_to_64 == NULL) {
        allow_high_allocs = false;
    }
#endif

    if (fd->is_memfile) {
        if (fd->readall) {
#if defined (__i386__)
            if (allow_high_allocs == true) {
                high_ret = (uintptr_t)fd->fd;
                return &high_ret;
            }
#endif
            return fd->fd;
        }
#if defined (UEFI) && defined (__x86_64__)
        if (!allow_high_allocs && (uintptr_t)fd->fd >= 0x100000000) {
            void *newptr = ext_mem_alloc_type(fd->size, type);
            memcpy(newptr, fd->fd, fd->size);
            pmm_free(fd->fd, fd->size);
            fd->fd = newptr;
        } else {
#endif
        memmap_alloc_range((uint64_t)(size_t)fd->fd, ALIGN_UP(fd->size, 4096), type, 0, true, false, false);
#if defined (UEFI) && defined (__x86_64__)
        }
#endif
        fd->readall = true;
#if defined (__i386__)
        if (allow_high_allocs == true) {
            high_ret = (uintptr_t)fd->fd;
            return &high_ret;
        }
#endif
        return fd->fd;
    } else {
        void *ret = ext_mem_alloc_type_aligned_mode(fd->size, type, 4096, allow_high_allocs);
#if defined (__i386__)
        if (allow_high_allocs == true) {
            high_ret = *(uint64_t *)ret;
            if (high_ret < 0x100000000) {
                ret = (void *)(uintptr_t)high_ret;
                goto low_ret;
            }
            void *pool = ext_mem_alloc(0x100000);
            for (size_t i = 0; i < fd->size; i += 0x100000) {
                size_t count;
                if (fd->size - i < 0x100000) {
                    count = fd->size - i;
                } else {
                    count = 0x100000;
                }
                fd->read(fd, pool, i, count);
                memcpy_to_64(high_ret + i, pool, count);
            }
            pmm_free(pool, 0x100000);
            return &high_ret;
        }
low_ret:
#endif
        fd->read(fd, ret, 0, fd->size);
        fd->close(fd);
        fd->fd = ret;
        fd->readall = true;
        fd->is_memfile = true;
#if defined (__i386__)
        if (allow_high_allocs == true) {
            return &high_ret;
        }
#endif
        return ret;
    }
}
