#ifndef COMPRESS__GZIP_H
#define COMPRESS__GZIP_H

#include <fs/file.h>

/* Wrap a gzip-compressed file handle in a decompressing layer.
 *
 * Returns a new file_handle whose read callback transparently
 * decompresses the data.  The returned handle takes ownership of
 * `compressed` and will close it when itself is closed.
 *
 * NOTE: ->size on the returned handle is set to UINT64_MAX as a
 * sentinel for "unknown". The gzip ISIZE trailer is decompressed-size
 * mod 2^32 and is not parsed here. Callers must drive ->read until
 * it returns 0 (end-of-stream) to discover the true size.
 *
 * Supports very fast sequential reads and random-access reads (with
 * an implicit rewind + skip penalty inherent to the gzip format).
 */
struct file_handle * gzip_open(struct file_handle * compressed);

#endif
