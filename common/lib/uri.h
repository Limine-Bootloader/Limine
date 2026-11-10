#ifndef LIB__URI_H
#define LIB__URI_H

#include <stdbool.h>
#include <fs/file.h>

bool uri_resolve(char *uri, char **resource, char **root, char **path, char **hash);

// uri_open resolves the URI, verifies the hash (if present) before anything
// parses the bytes, gzip-decodes (if the resource is prefixed with `$`), and
// returns a memfile (is_memfile=true, readall=true) whose payload has been
// placed in memory of the requested `type`.
//
// When `allow_high_mem` is true and the target architecture is i386, the
// buffer may end up above 4 GiB; in that case the returned handle has
// is_high_mem=true, fd=NULL, and load_addr_64 holding the physical address.
// Otherwise load_addr_64 == (uintptr_t)fd. On i386 the two memcpy callbacks
// are used only when allow_high_mem is true; pass NULL otherwise.
struct file_handle *uri_open(char *uri, uint32_t type, bool allow_high_mem
#if defined(__i386__)
    , void (*memcpy_to_64)(uint64_t dst, void *src, size_t count)
    , void (*memcpy_from_64)(void *dst, uint64_t src, size_t count)
#endif
);

// Same as uri_open, for a resource the boot can do without: a problem that
// would return to the menu is printed instead and NULL is returned, and so is
// a hash mismatch, whatever hash_mismatch_panic says.
struct file_handle *uri_open_optional(char *uri, uint32_t type, bool allow_high_mem
#if defined(__i386__)
    , void (*memcpy_to_64)(uint64_t dst, void *src, size_t count)
    , void (*memcpy_from_64)(void *dst, uint64_t src, size_t count)
#endif
);

#endif
