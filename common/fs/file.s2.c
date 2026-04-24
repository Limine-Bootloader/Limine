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
            goto err;
        }
        pmm_free(filename_new, filename_new_len);
        return ret;
    }

    if ((ret = iso9660_open(part, filename)) != NULL) {
        goto success;
    }
    if ((ret = fat32_open(part, filename)) != NULL) {
        goto success;
    }

err:
    pmm_free(filename_new, filename_new_len);
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

uint64_t fread(struct file_handle *fd, void *buf, uint64_t loc, uint64_t count) {
    if (fd->is_memfile) {
        if (fd->is_high_mem) {
            panic(false, "fread: memfile resides above 4 GiB; caller must use load_addr_64 directly");
        }
        if (loc >= fd->size || count > fd->size - loc) {
            panic(false, "fread: attempted out of bounds read");
        }
        memcpy(buf, fd->fd + loc, count);
        return count;
    } else {
        return fd->read(fd, buf, loc, count);
    }
}
