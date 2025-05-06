#ifndef LIB__URI_H__
#define LIB__URI_H__

#include <stdbool.h>
#include <fs/file.h>

#if defined (UEFI) && defined (__x86_64__)
extern bool uri_open_allow_high;
#endif

bool uri_resolve(char *uri, char **resource, char **root, char **path, char **hash);
struct file_handle *uri_open(char *uri);

#endif
