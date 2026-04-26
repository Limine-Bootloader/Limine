#ifndef LIB__QOI_H__
#define LIB__QOI_H__

#include <stdint.h>
#include <stddef.h>

/*  Decodes a QOI image (https://qoiformat.org) from `src` (size `src_size`)
    into a freshly allocated XRGB8 pixel buffer. Each output pixel is a 32-bit
    little-endian word laid out as 0x00RRGGBB; the QOI alpha is dropped.
    On success, returns the buffer and writes the decoded width/height into
    *out_w / *out_h. Returns NULL on malformed input. The returned buffer
    must be released with qoi_free().  */
uint8_t *qoi_decode(const void *src, size_t src_size,
    int *out_w, int *out_h);

/*  Releases a buffer returned by qoi_decode(). NULL is accepted.  */
void qoi_free(uint8_t *buf);

#endif
