#ifndef FS__FAT32_H
#define FS__FAT32_H

#include <lib/part.h>
#include <fs/file.h>

char *fat32_get_label(struct volume *part);

struct file_handle *fat32_open(struct volume *part, const char *path);

#endif
