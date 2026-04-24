/* embeddable gzip decoder: Copyright (C) 2026 Kamila Szewczyk <k@iczelia.net>
 * limine: Copyright (C) 2019-2026 Mintsuki and contributors.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef COMPRESS__GZIP_H__
#define COMPRESS__GZIP_H__

#include <fs/file.h>

/* Check if a file handle points to gzip-compressed data (0x1F 0x8B magic). */
bool gzip_check(struct file_handle * fd);

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
