/* Limine glue around pdgzip: transparent gzip decompression layer over a
 * file_handle.  The underlying decoder lives in common/compress/pdgzip.c
 * (imported by ./bootstrap from the upstream iczelia/pdgzip repo); this
 * file only wires pdgzip's streaming read-callback API into Limine's
 * file_handle abstraction and adds support for random-access reads via
 * rewind-and-skip.
 *
 * Copyright (C) 2019-2026 Mintsuki and contributors.
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

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <mm/pmm.h>
#include <compress/gzip.h>
#include <compress/pdgzip.h>

struct gzip_handle {
  struct file_handle * source;     /*  compressed file (owned)  */
  pdgzip_t           * gz;         /*  decoder backed by `scratch`  */
  void               * scratch;    /*  pdgzip scratch buffer  */
  size_t               scratch_sz;
  uint64_t             src_pos;    /*  next byte to pull from `source`  */
  uint64_t             dec_pos;    /*  current decompressed stream offset  */
};

/*  pdgzip read callback: pull up to `len` bytes from the compressed source
    starting at gh->src_pos.  A short read (including zero) signals EOF to
    the decoder, which is correct at the end of the file.  */
static size_t gz_source_read(void * user, void * buf, size_t len) {
  struct gzip_handle * gh = user;
  uint64_t avail = gh->source->size - gh->src_pos;
  if ((uint64_t)len > avail) len = (size_t)avail;
  if (len == 0) return 0;
  size_t got = fread(gh->source, buf, gh->src_pos, len);
  gh->src_pos += got;
  return got;
}

/*  (Re)initialize the decoder for a fresh pass over the compressed stream.
    pdgzip_init zeroes its own scratch, so we only need to reset our own
    bookkeeping.  */
static void gz_reset(struct gzip_handle * gh) {
  pdgzip_cfg_t cfg = { .read = gz_source_read, .user = gh, .concat = 0 };
  gh->src_pos = 0;
  gh->dec_pos = 0;
  gh->gz = pdgzip_init(gh->scratch, &cfg);
}

static uint64_t gzip_read(struct file_handle * file, void * buf, uint64_t loc, uint64_t count) {
  struct gzip_handle * gh = file->fd;
  /*  Rewind on backward seeks.  */
  if (loc < gh->dec_pos) gz_reset(gh);
  /*  Skip forward to reach the requested offset.  EOS during seek means
      the requested location is past end-of-stream - return 0 bytes.  */
  while (gh->dec_pos < loc) {
    uint8_t discard[4096];
    uint64_t gap = loc - gh->dec_pos;
    size_t chunk = gap > sizeof(discard) ? sizeof(discard) : (size_t)gap;
    int64_t n = pdgzip_read(gh->gz, discard, chunk);
    if (n < 0) panic(false, "gzip: decompression error during seek");
    if (n == 0) return 0;
    gh->dec_pos += (uint64_t)n;
  }
  /*  Decompress the requested data.  */
  uint8_t * dst = buf;
  uint64_t remaining = count;
  while (remaining > 0) {
    size_t chunk = remaining > 65536 ? 65536 : (size_t)remaining;
    int64_t n = pdgzip_read(gh->gz, dst, chunk);
    if (n < 0) panic(false, "gzip: decompression error");
    if (n == 0) break;
    dst += n;
    remaining -= (uint64_t)n;
    gh->dec_pos += (uint64_t)n;
  }
  return count - remaining;
}

static void gzip_close(struct file_handle * file) {
  struct gzip_handle * gh = file->fd;
  fclose(gh->source);
  pmm_free(gh->scratch, gh->scratch_sz);
  pmm_free(gh, sizeof(struct gzip_handle));
}

bool gzip_check(struct file_handle * fd) {
  if (fd->size < 18) return false;
  uint8_t magic[2];  fread(fd, magic, 0, 2);
  return magic[0] == 0x1F && magic[1] == 0x8B;
}

struct file_handle * gzip_open(struct file_handle * compressed) {
  /*  The decompressed size is not known up front.  The 4-byte ISIZE trailer
      is unreliable (modulo 2^32, spec defect) and callers must instead
      drain until gzip_read returns 0 bytes (EOS).  Advertise an unknown
      size via UINT64_MAX.  */
  struct gzip_handle * gh = ext_mem_alloc(sizeof(struct gzip_handle));
  gh->source     = compressed;
  gh->scratch_sz = pdgzip_state_size();
  gh->scratch    = ext_mem_alloc(gh->scratch_sz);
  gz_reset(gh);
  /*  Depends on ext_mem_alloc returning zeroed memory.  */
  struct file_handle * ret = ext_mem_alloc(sizeof(struct file_handle));
  ret->fd = gh;
  ret->read = (void *) gzip_read;
  ret->close = (void *) gzip_close;
  ret->size = UINT64_MAX;
  ret->vol = compressed->vol;
  if (compressed->path != NULL && compressed->path_len > 0) {
    ret->path = ext_mem_alloc(compressed->path_len);
    memcpy(ret->path, compressed->path, compressed->path_len);
    ret->path_len = compressed->path_len;
  }
#if defined (UEFI)
  ret->efi_part_handle = compressed->efi_part_handle;
#endif
  ret->pxe = compressed->pxe;
  ret->pxe_ip = compressed->pxe_ip;
  ret->pxe_port = compressed->pxe_port;
  return ret;
}
