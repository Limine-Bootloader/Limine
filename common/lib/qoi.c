#include <stdint.h>
#include <stddef.h>
#include <lib/qoi.h>
#include <lib/misc.h>
#include <mm/pmm.h>

#define QOI_OP_INDEX 0x00
#define QOI_OP_DIFF  0x40
#define QOI_OP_LUMA  0x80
#define QOI_OP_RUN   0xc0
#define QOI_OP_RGB   0xfe
#define QOI_OP_RGBA  0xff
#define QOI_OP_MASK  0xc0
#define QOI_HEADER_SIZE  14
#define QOI_PADDING_SIZE  8
#define QOI_MAX_DIM    65536u
#define QOI_MAX_PIXELS ((size_t)400000000)
#define QOI_HASH(r, g, b, a) \
    ((((unsigned)(r) * 3u) + ((unsigned)(g) * 5u) + \
        ((unsigned)(b) * 7u) + ((unsigned)(a) * 11u)) & 63u)

static uint32_t qoi_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/*  The decoded buffer is laid out as [total_bytes][padding to 16][pixels...].
    The 16-byte header lets qoi_free() recover the original allocation size
    without the caller having to track it.  */
static uint32_t *qoi_alloc_xrgb(size_t pixels) {
    size_t bytes = CHECKED_MUL(pixels, (size_t)4, return NULL);
    size_t total = CHECKED_ADD(bytes, (size_t)16, return NULL);
    void *raw = ext_mem_alloc(total);
    *(size_t *) raw = total;
    return (uint32_t *)((uint8_t *) raw + 16);
}

void qoi_free(uint8_t *buf) {
    if (buf) { uint8_t *raw = buf - 16;  pmm_free(raw, *(size_t *) raw); }
}

uint8_t *qoi_decode(const void *src, size_t src_size,
            int *out_w, int *out_h) {
    if (!src || src_size < QOI_HEADER_SIZE + QOI_PADDING_SIZE) return NULL;
    const uint8_t * p = src;
    if (p[0] != 'q' || p[1] != 'o' || p[2] != 'i' || p[3] != 'f')
        return NULL;
    uint32_t w = qoi_be32(p + 4), h = qoi_be32(p + 8);
    uint8_t channels = p[12];  /* p[13] is the colorspace tag, ignored. */
    if (w == 0 || h == 0 || w > QOI_MAX_DIM || h > QOI_MAX_DIM) return NULL;
    if (channels != 3 && channels != 4) return NULL;
    size_t pixels = CHECKED_MUL((size_t)w, (size_t)h, return NULL);
    if (pixels > QOI_MAX_PIXELS) return NULL;
    uint32_t *out = qoi_alloc_xrgb(pixels);
    if (out == NULL) return NULL;
    uint32_t index[64] = { 0 }, v;  int run = 0, dg;
    uint8_t r = 0, g = 0, b = 0, a = 0xff, b2;
    size_t pos = QOI_HEADER_SIZE, end = src_size - QOI_PADDING_SIZE;
    for (size_t px = 0; px < pixels; px++) {
        if (run > 0) run--; else {
            if (pos >= end) goto fail;
            uint8_t op = p[pos++];
            if (op == QOI_OP_RGB) {
                if (end - pos < 3) goto fail;
                r = p[pos++]; g = p[pos++]; b = p[pos++];
            } else if (op == QOI_OP_RGBA) {
                if (end - pos < 4) goto fail;
                r = p[pos++]; g = p[pos++]; b = p[pos++]; a = p[pos++];
            } else switch (op & QOI_OP_MASK) {
                case QOI_OP_INDEX:
                    v = index[op & 0x3f];
                    r = v;  g = v >> 8;  b = v >> 16;  a = v >> 24;
                    break;
                case QOI_OP_DIFF:
                    r += (int)((op >> 4) & 3u) - 2;
                    g += (int)((op >> 2) & 3u) - 2;
                    b += (int)( op       & 3u) - 2;
                    break;
                case QOI_OP_LUMA:
                    if (pos >= end) goto fail;
                    b2 = p[pos++];
                    dg = (int)(op & 0x3f) - 32;
                    r += dg + (int)((b2 >> 4) & 0xf) - 8;
                    g += dg;
                    b += dg + (int)( b2       & 0xf) - 8;
                    break;
                case QOI_OP_RUN: run = op & 0x3f; break;
            }
            index[QOI_HASH(r, g, b, a)] =
              (uint32_t)r        | ((uint32_t)g << 8) |
             ((uint32_t)b << 16) | ((uint32_t)a << 24);
        }
        out[px] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    *out_w = (int)w;  *out_h = (int)h;  return (uint8_t *) out;
fail:
    qoi_free((uint8_t *) out);  return NULL;
}
