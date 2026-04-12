#ifndef FS__FILE_H__
#define FS__FILE_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/part.h>
#if defined (UEFI)
#  include <efi.h>
#endif

extern bool case_insensitive_fopen;

bool fs_get_guid(struct guid *guid, struct volume *part);
char *fs_get_label(struct volume *part);

struct file_handle {
    bool       is_memfile;
    bool       readall;
    bool       is_high_mem;
    struct volume *vol;
    char      *path;
    size_t     path_len;
    void      *fd;
    uint64_t (*read)(void *fd, void *buf, uint64_t loc, uint64_t count);
    void     (*close)(void *fd);
    uint64_t   size;
    uint64_t   load_addr_64;
#if defined (UEFI)
    EFI_HANDLE efi_part_handle;
#endif
    bool pxe;
    uint32_t pxe_ip;
    uint16_t pxe_port;
};

struct file_handle *fopen(struct volume *part, const char *filename);
uint64_t fread(struct file_handle *fd, void *buf, uint64_t loc, uint64_t count);
void fclose(struct file_handle *fd);

#endif
