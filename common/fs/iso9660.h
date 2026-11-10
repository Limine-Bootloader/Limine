#ifndef FS__ISO9660_H
#define FS__ISO9660_H

#include <stdint.h>
#include <lib/part.h>
#include <fs/file.h>

struct file_handle *iso9660_open(struct volume *vol, const char *path);
char *iso9660_get_label(struct volume *vol);

#endif
