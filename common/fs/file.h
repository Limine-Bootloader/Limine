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

enum {
    DIR_ENTRY_TYPE_FILE,
    DIR_ENTRY_TYPE_DIRECTORY,
};

#define DIR_ENTRY_NAME_LEN 384
struct dir_entry {
    char name[DIR_ENTRY_NAME_LEN];
    int type;
};

struct file_handle {
    bool       is_memfile;
    bool       readall;
    struct volume *vol;
    char      *path;
    size_t     path_len;
    void      *fd;
    void     (*read)(void *fd, void *buf, uint64_t loc, uint64_t count);
    void     (*close)(void *fd);
    void    *(*readdir)(void *fd, size_t *out_size);
    uint64_t   size;
#if defined (UEFI)
    EFI_HANDLE efi_part_handle;
#endif
    bool pxe;
    uint32_t pxe_ip;
    uint16_t pxe_port;
};

struct file_handle *fopen(struct volume *part, const char *filename);
void fread(struct file_handle *fd, void *buf, uint64_t loc, uint64_t count);
void fclose(struct file_handle *fd);
struct dir_entry *freaddir(struct file_handle *fd, size_t *out_size);
void *freadall(struct file_handle *fd, uint32_t type);
void *freadall_mode(struct file_handle *fd, uint32_t type, bool allow_high_allocs
#if defined (__i386__)
    , void (*memcpy_to_64)(uint64_t dst, void *src, size_t count)
#endif
);

#endif
