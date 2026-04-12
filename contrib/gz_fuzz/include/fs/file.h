#ifndef FS__FILE_H__
#define FS__FILE_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*  Rename to dodge libc's stdio.h fread/fclose.  Callers that also want
    libc stdio must include <stdio.h> BEFORE this header.  */
#define fread lim_fread
#define fclose lim_fclose

struct volume;

struct file_handle {
    bool       is_memfile;
    bool       readall;
    struct volume *vol;
    char      *path;
    size_t     path_len;
    void      *fd;
    void     (*read)(void *fd, void *buf, uint64_t loc, uint64_t count);
    void     (*close)(void *fd);
    uint64_t   size;
    bool       pxe;
    uint32_t   pxe_ip;
    uint16_t   pxe_port;
};

void fread(struct file_handle *fd, void *buf, uint64_t loc, uint64_t count);
void fclose(struct file_handle *fd);

#endif
