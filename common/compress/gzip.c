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

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <mm/pmm.h>
#include <compress/gzip.h>

/*  Various tuning macros. Most are fixed by the DEFLATE RFC,
    but different values of HUFF_BITS and INBUF_SIZE are permissible.  */
#define HUFF_BITS            9
#define INBUF_SIZE           (1 << 14)

#define HUFF_SIZE            (1 << HUFF_BITS)
#define HUFF_MASK            (HUFF_SIZE - 1)

#define MAX_LITLEN_CODES     288
#define MAX_DIST_CODES       32
#define MAX_CL_CODES         19
#define MAX_BITS             15

#define WINDOW_SIZE          32768

/*  Huffman Tables  */
typedef uint32_t huff_entry_t;

#define HUFF_SYM(e)          ((e) & 0xFFFF)
#define HUFF_LEN(e)          (((e) >> 16) & 0xF)
#define HUFF_REDIRECT(e)     (((e) >> 20) & 1)

static inline huff_entry_t make_entry(uint16_t sym, uint8_t bits) {
  return (uint32_t)sym | ((uint32_t)bits << 16);
}

static inline huff_entry_t make_redirect(uint8_t sub_bits, uint32_t offset) {
  return (offset & 0xFFFF) | ((uint32_t)(sub_bits & 0xF) << 16) | (1u << 20);
}

#define HUFF_CODE_EXTRA      (1 << (MAX_BITS - HUFF_BITS + 1))
#define HUFF_TAB_MAX         (HUFF_SIZE + HUFF_CODE_EXTRA * MAX_LITLEN_CODES)

typedef struct { huff_entry_t table[HUFF_TAB_MAX];  int used; } huff_table_t;

/*  Bit-wise I/O wrapping the original Limine handle.  */
typedef struct {
  struct file_handle * fh;
  uint64_t fh_pos;
  uint8_t buf[INBUF_SIZE];
  int buf_pos, buf_end;
  uint64_t bits;
  int nbits;
  int eof;
} bitreader_t;

static void br_init(bitreader_t * br, struct file_handle * fh) {
  memset(br, 0, sizeof(bitreader_t));
  br->fh = fh;
}

static int br_refill_buf(bitreader_t * br) {
  uint64_t avail = br->fh->size - br->fh_pos;
  if (avail == 0) {
    return br->buf_end = br->buf_pos = 0;
  }
  size_t to_read = INBUF_SIZE;
  if (to_read > avail) to_read = (size_t)avail;
  fread(br->fh, br->buf, br->fh_pos, to_read);
  br->fh_pos += to_read;
  br->buf_end = (int)to_read;
  br->buf_pos = 0;
  return 1;
}
static inline void br_need(bitreader_t * br, int n) {
  while (br->nbits < n) {
    if (br->buf_pos >= br->buf_end) {
      if (!br_refill_buf(br)) {
        br->eof = 1;  br->nbits = 64;  return;
      }
    }
    br->bits |= (uint64_t)br->buf[br->buf_pos++] << br->nbits;
    br->nbits += 8;
  }
}

static inline uint64_t br_peek(bitreader_t * br, int n) {
  return br->bits & (((uint64_t)1 << n) - 1);
}

static inline void br_drop(bitreader_t * br, int n) {
  br->bits >>= n;  br->nbits -= n;
  if (br->nbits < 0) br->nbits = 0;
}

static inline uint64_t br_read(bitreader_t * br, int n) {
  br_need(br, n);
  uint64_t v = br_peek(br, n);
  br_drop(br, n);
  return v;
}

static inline void br_align(bitreader_t * br) {
  br_drop(br, br->nbits & 7);
}

static inline uint16_t br_read_u16(bitreader_t * br) {
  uint16_t lo = (uint16_t) br_read(br, 8);
  uint16_t hi = (uint16_t) br_read(br, 8);
  return lo | (hi << 8);
}

static inline uint32_t br_read_u32(bitreader_t * br) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++)
    v |= (uint32_t)br_read(br, 8) << (i * 8);
  return v;
}

/*  CRC-32 decompression code, slicing-by-4.
    The 4 KiB table lives in bootloader-reclaimable memory so the
    booted kernel gets it back via the memory map.  It is allocated
    once on first use and reused across every gzip stream for the
    lifetime of the bootloader.  */
static uint32_t (*crc_table)[256] = NULL;

static void crc32_init_table(void) {
  if (crc_table != NULL) return;
  crc_table = ext_mem_alloc(sizeof(uint32_t) * 4 * 256);
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int j = 0; j < 8; j++)
      c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0);
    crc_table[0][i] = c;
  }
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = crc_table[0][i];
    for (int k = 1; k < 4; k++) {
      c = crc_table[0][c & 0xFF] ^ (c >> 8);
      crc_table[k][i] = c;
    }
  }
}

static uint32_t crc32_update(uint32_t crc, const uint8_t * data, size_t len) {
  for (; len >= 4; data += 4, len -= 4) {
    crc ^= (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
          ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    crc = crc_table[3][(crc      ) & 0xFF] ^
          crc_table[2][(crc >>  8) & 0xFF] ^
          crc_table[1][(crc >> 16) & 0xFF] ^
          crc_table[0][(crc >> 24) & 0xFF];
  }
  while (len--)
    crc = crc_table[0][(crc ^ *data++) & 0xFF] ^ (crc >> 8);
  return crc;
}

/*  Checked Huffman table building, decoder.  */
static int huff_build(huff_table_t * ht, const uint8_t * lengths, int count) {
  uint16_t bl_count[MAX_BITS + 1] = { 0 }, next_code[MAX_BITS + 1];
  uint16_t codes[MAX_LITLEN_CODES];

  int max_len = 0;
  for (int i = 0; i < count; i++) {
    if (lengths[i] > MAX_BITS) return -1;
    bl_count[lengths[i]]++;
    if (lengths[i] > max_len) max_len = lengths[i];
  }
  bl_count[0] = 0;

  uint32_t code = 0;
  for (int bits = 1; bits <= max_len; bits++) {
    code = (code + bl_count[bits - 1]) << 1;   /* Canonical Codes.  */
    next_code[bits] = (uint16_t)code;
  }
  if (max_len > 0 && code + bl_count[max_len] > (1u << max_len))
    return -1;  /*  Kraft condition failed.  */

  for (int i = 0; i < count; i++) {
    if (lengths[i] == 0) continue;
    codes[i] = next_code[lengths[i]]++;
  }

  ht->used = HUFF_SIZE;
  memset(ht->table, 0, HUFF_SIZE * sizeof(huff_entry_t));

  for (int sym = 0; sym < count; sym++) {
    int len = lengths[sym];
    if (len == 0) continue;

    if (len <= HUFF_BITS) {
      uint16_t entry_idx = 0;
      for (int b = 0; b < len; b++)
        entry_idx |= ((codes[sym] >> (len - 1 - b)) & 1) << b;

      int step = 1 << len;
      for (int idx = entry_idx; idx < HUFF_SIZE; idx += step)
        ht->table[idx] = make_entry((uint16_t)sym, (uint8_t)len);
    }
  }

  if (max_len > HUFF_BITS) {
    int sub_bits_needed[HUFF_SIZE];
    memset(sub_bits_needed, 0, sizeof(sub_bits_needed));

    for (int sym = 0; sym < count; sym++) {
      int len = lengths[sym];
      if (len <= HUFF_BITS) continue;

      uint16_t rev = 0;
      for (int b = 0; b < len; b++)
        rev |= ((codes[sym] >> (len - 1 - b)) & 1) << b;

      int primary = rev & HUFF_MASK, extra = len - HUFF_BITS;
      if (extra > sub_bits_needed[primary])
        sub_bits_needed[primary] = extra;
    }

    int sub_offsets[HUFF_SIZE];
    memset(sub_offsets, -1, sizeof(sub_offsets));

    for (int p = 0; p < HUFF_SIZE; p++) {
      if (sub_bits_needed[p] == 0) continue;
      int sub_sz = 1 << sub_bits_needed[p];
      if (ht->used + sub_sz > HUFF_TAB_MAX) return -1;
      sub_offsets[p] = ht->used;
      memset(&ht->table[ht->used], 0, sub_sz * sizeof(huff_entry_t));
      ht->table[p] = make_redirect((uint8_t)sub_bits_needed[p],
                     (uint32_t)ht->used);
      ht->used += sub_sz;
    }

    for (int sym = 0; sym < count; sym++) {
      int len = lengths[sym];
      if (len <= HUFF_BITS) continue;

      uint16_t rev = 0;
      for (int b = 0; b < len; b++)
        rev |= ((codes[sym] >> (len - 1 - b)) & 1) << b;

      int primary = rev & HUFF_MASK, extra_bits = len - HUFF_BITS;
      int sub_idx = (rev >> HUFF_BITS) & ((1 << sub_bits_needed[primary]) - 1);
      int off = sub_offsets[primary];

      int step = 1 << extra_bits, sub_sz = 1 << sub_bits_needed[primary];
      for (int idx = sub_idx; idx < sub_sz; idx += step)
        ht->table[off + idx] = make_entry((uint16_t)sym, (uint8_t)len);
    }
  }

  return 0;
}


static inline int huff_decode(bitreader_t * br, const huff_table_t * ht) {
  br_need(br, MAX_BITS);
  uint64_t peek = br_peek(br, MAX_BITS);
  huff_entry_t e = ht->table[peek & HUFF_MASK];
  if (HUFF_REDIRECT(e)) {
    uint32_t off = HUFF_SYM(e), sub_bits = HUFF_LEN(e);
    unsigned idx = (unsigned)((peek >> HUFF_BITS) & ((1u << sub_bits) - 1));
    e = ht->table[off + idx];
  }
  int len = HUFF_LEN(e);
  if (len == 0) return -1;
  br_drop(br, len);
  return (int) HUFF_SYM(e);
}

/*  Length/distance tables, decoder state machine.  */
static const uint16_t len_base[29] = {
    3,   4,   5,   6,   7,   8,   9,  10,  11,  13,
   15,  17,  19,  23,  27,  31,  35,  43,  51,  59,
   67,  83,  99, 115, 131, 163, 195, 227, 258
};
static const uint8_t len_extra[29] = {
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
  1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
  4, 4, 4, 4, 5, 5, 5, 5, 0
};

static const uint16_t dist_base[30] = {
  1,     2,     3,     4,     5,     7,     9,      13,    17,     25,
  33,    49,    65,    97,    129,   193,   257,   385,   513,    769,
  1025,  1537,  2049,  3073,  4097,  6145,  8193, 12289, 16385, 24577
};
static const uint8_t dist_extra[30] = {
  0,  0,  0,  0,  1,  1,  2,  2,  3,  3,
  4,  4,  5,  5,  6,  6,  7,  7,  8,  8,
  9,  9, 10, 10, 11, 11, 12, 12, 13, 13
};

enum {
  S_HEADER, S_BLOCK_HDR, S_STORED_HDR, S_STORED_DATA, S_DYNAMIC_HDR,
  S_DECODE, S_MATCH, S_TRAILER,
  S_DONE, S_ERROR
};

typedef struct {
  bitreader_t br;
  uint8_t window[WINDOW_SIZE * 2];
  uint32_t wpos, crc, total;
  int state, bfinal;
  uint16_t store_remaining;
  const huff_table_t * ht_lit, * ht_dist;
  huff_table_t ht_litbuf, ht_distbuf;
  uint16_t match_len, match_dist, match_pos;
} gz_state_t;

/*  Rebuild the RFC1951 fixed-Huffman tables into the per-stream buffers.
    Done lazily on each BTYPE=01 block: fixed blocks are rare in modern
    gzip streams, and this keeps the tables out of .bss entirely.  */
static void build_fixed_tables(gz_state_t * gz) {
  uint8_t ll[288];  int i;
  for (i =   0; i <= 143; i++) ll[i] = 8;
  for (i = 144; i <= 255; i++) ll[i] = 9;
  for (i = 256; i <= 279; i++) ll[i] = 7;
  for (i = 280; i <= 287; i++) ll[i] = 8;
  huff_build(&gz->ht_litbuf, ll, 288);
  uint8_t dd[32];
  for (i = 0; i < 32; i++) dd[i] = 5;
  huff_build(&gz->ht_distbuf, dd, 32);
}

/*  Hardened Gzip header handling.  */
#define GZ_MAGIC1            0x1F
#define GZ_MAGIC2            0x8B
#define GZ_METHOD_DEFLATE    8
#define FHCRC                (1 << 1)
#define FEXTRA               (1 << 2)
#define FNAME                (1 << 3)
#define FCOMMENT             (1 << 4)

static int parse_gz_header(gz_state_t * gz) {
  bitreader_t * br = &gz->br;
  uint8_t id1 = (uint8_t)br_read(br, 8);
  uint8_t id2 = (uint8_t)br_read(br, 8);
  if (id1 != GZ_MAGIC1 || id2 != GZ_MAGIC2)
    return -1;
  uint8_t method = (uint8_t)br_read(br, 8);
  if (method != GZ_METHOD_DEFLATE)
    return -1;
  uint8_t flags = (uint8_t)br_read(br, 8);
  for (int i = 0; i < 6; i++)
    br_read(br, 8); /*  Skip timestamp, xflags, OS.  */
  if (flags & FEXTRA) {
    uint16_t xlen = br_read_u16(br);
    for (uint16_t i = 0; i < xlen; i++)
      br_read(br, 8);
  }
  if (flags & FNAME) {
    while ((uint8_t)br_read(br, 8) != 0);
  }
  if (flags & FCOMMENT) {
    while ((uint8_t)br_read(br, 8) != 0);
  }
  if (flags & FHCRC) {  /*  Skip *header* CRC.  */
    br_read(br, 8);  br_read(br, 8);
  }
  return 0;
}

/*  Gzip format table parser.  */
static const int cl_order[MAX_CL_CODES] = {
  16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int build_dynamic_tables(gz_state_t * gz) {
  bitreader_t * br = &gz->br;
  int hlit  = (int)br_read(br, 5) + 257;
  int hdist = (int)br_read(br, 5) + 1;
  int hclen = (int)br_read(br, 4) + 4;
  if (hlit > 286 || hdist > 30) return -1;
  uint8_t cl_lengths[MAX_CL_CODES] = {0};
  for (int i = 0; i < hclen; i++)
    cl_lengths[cl_order[i]] = (uint8_t)br_read(br, 3);
  huff_table_t * ht_cl = ext_mem_alloc(sizeof(huff_table_t));
  if (huff_build(ht_cl, cl_lengths, MAX_CL_CODES) < 0) {
    pmm_free(ht_cl, sizeof(huff_table_t));  return -1;
  }
  int total = hlit + hdist, idx = 0;
  uint8_t all_lengths[MAX_LITLEN_CODES + MAX_DIST_CODES] = { 0 };
  while (idx < total) {
    int sym = huff_decode(br, ht_cl);
    if (sym < 0) { pmm_free(ht_cl, sizeof(huff_table_t));  return -1; }
    if (sym < 16) {
      all_lengths[idx++] = (uint8_t)sym;
    } else if (sym == 16) {
      if (idx == 0) { pmm_free(ht_cl, sizeof(huff_table_t));  return -1; }
      int rep = (int)br_read(br, 2) + 3;
      uint8_t prev = all_lengths[idx - 1];
      for (int i = 0; i < rep && idx < total; i++)
        all_lengths[idx++] = prev;
    } else if (sym == 17) {
      int rep = (int)br_read(br, 3) + 3;
      for (int i = 0; i < rep && idx < total; i++)
        all_lengths[idx++] = 0;
    } else if (sym == 18) {
      int rep = (int)br_read(br, 7) + 11;
      for (int i = 0; i < rep && idx < total; i++)
        all_lengths[idx++] = 0;
    } else {
      pmm_free(ht_cl, sizeof(huff_table_t));  return -1;
    }
  }
  pmm_free(ht_cl, sizeof(huff_table_t));
  if (huff_build(&gz->ht_litbuf, all_lengths, hlit) < 0) return -1;
  if (huff_build(&gz->ht_distbuf, all_lengths + hlit, hdist) < 0) return -1;
  gz->ht_lit  = &gz->ht_litbuf;
  gz->ht_dist = &gz->ht_distbuf;
  return 0;
}

/*  Pull decompressed bytes per the state machine.  */
#define WINDOW_FLUSH(crc, win)  do {                   \
  (crc) = crc32_update((crc), (win) + WINDOW_SIZE, WINDOW_SIZE);   \
  memcpy((win), (win) + WINDOW_SIZE, WINDOW_SIZE);           \
} while (0)

static int64_t gz_decompress(gz_state_t * gz, void * dst, size_t n) {
  uint8_t * out = dst;
  size_t written = 0;
  while (written < n) {
    switch (gz->state) {
    case S_HEADER:
      if (parse_gz_header(gz) < 0) {
        gz->state = S_ERROR;  return -1;
      }
      gz->state = S_BLOCK_HDR;
      break;
    
    case S_BLOCK_HDR: {
      gz->bfinal = (int)br_read(&gz->br, 1);
      int btype  = (int)br_read(&gz->br, 2);
      switch (btype) {
      case 0:
        gz->state = S_STORED_HDR;
        break;
      case 1:
        build_fixed_tables(gz);
        gz->ht_lit  = &gz->ht_litbuf;
        gz->ht_dist = &gz->ht_distbuf;
        gz->state   = S_DECODE;
        break;
      case 2:
        gz->state = S_DYNAMIC_HDR;
        break;
      default:
        gz->state = S_ERROR;  return -1;
      }
      break;
    }

    case S_STORED_HDR: {
      br_align(&gz->br);
      uint16_t len  = br_read_u16(&gz->br);
      uint16_t nlen = br_read_u16(&gz->br);
      if (len != (uint16_t)~nlen) {
        gz->state = S_ERROR;  return -1;
      }
      gz->store_remaining = len;
      gz->state = S_STORED_DATA;
      break;
    }

    case S_STORED_DATA: {
      while (gz->store_remaining > 0 && written < n) {
        if (gz->wpos >= WINDOW_SIZE * 2) {
          WINDOW_FLUSH(gz->crc, gz->window);
          gz->wpos = WINDOW_SIZE;
        }
        uint8_t b = (uint8_t)br_read(&gz->br, 8);
        out[written++] = b;
        gz->window[gz->wpos++] = b;
        gz->total++;
        gz->store_remaining--;
      }
      if (gz->store_remaining == 0)
        gz->state = gz->bfinal ? S_TRAILER : S_BLOCK_HDR;
      break;
    }

    case S_DYNAMIC_HDR:
      if (build_dynamic_tables(gz) < 0) {
        gz->state = S_ERROR;  return -1;
      }
      gz->state = S_DECODE;
      break;

    case S_DECODE: {
      uint32_t wpos  = gz->wpos, total = gz->total;
      uint8_t * window = gz->window;
      while (written < n && gz->state == S_DECODE) {
        if (wpos >= WINDOW_SIZE * 2) {
          gz->crc = crc32_update(gz->crc, window + WINDOW_SIZE, WINDOW_SIZE);
          memcpy(window, window + WINDOW_SIZE, WINDOW_SIZE);
          wpos = WINDOW_SIZE;
        }
        int sym = huff_decode(&gz->br, gz->ht_lit);
        if (sym >= 0 && sym < 256) {
          uint8_t b = (uint8_t)sym;
          out[written++] = window[wpos++] = b;
          total++;
        } else if (sym == 256) {
          gz->state = gz->bfinal ? S_TRAILER : S_BLOCK_HDR;
        } else if (sym < 0 || sym > 285) {
          gz->wpos = wpos; gz->total = total;
          gz->state = S_ERROR;  return -1;
        } else {
          int li = sym - 257;
          gz->match_len = len_base[li] +
            (uint16_t)br_read(&gz->br, len_extra[li]);
          int di = huff_decode(&gz->br, gz->ht_dist);
          if (di < 0 || di >= 30) {
            gz->wpos = wpos; gz->total = total;
            gz->state = S_ERROR;  return -1;
          }
          gz->match_dist = dist_base[di] +
            (uint16_t)br_read(&gz->br, dist_extra[di]);
          gz->match_pos = 0;
          gz->state = S_MATCH;
        }
      }
      gz->wpos  = wpos;
      gz->total = total;
      break;
    }

    case S_MATCH: {
      uint32_t wpos  = gz->wpos, total = gz->total;
      uint8_t * window = gz->window;
      uint16_t dist = gz->match_dist;
      while (gz->match_pos < gz->match_len && written < n) {
        if (wpos >= WINDOW_SIZE * 2) {
          gz->crc = crc32_update(gz->crc, window + WINDOW_SIZE, WINDOW_SIZE);
          memcpy(window, window + WINDOW_SIZE, WINDOW_SIZE);
          wpos = WINDOW_SIZE;
        }
        size_t remaining = gz->match_len - gz->match_pos, chunk = remaining;
        size_t buf_space = n - written,  win_space = WINDOW_SIZE * 2 - wpos;
        if (chunk > buf_space) chunk = buf_space;
        if (chunk > win_space) chunk = win_space;
        if (dist >= chunk) {
          memcpy(window + wpos, window + wpos - dist, chunk);
          memcpy(out + written, window + wpos, chunk);
          wpos += (uint32_t)chunk;
          written += chunk;
          total += (uint32_t)chunk;
          gz->match_pos += (uint16_t)chunk;
        } else {
          uint8_t b = window[wpos - dist];
          out[written++] = window[wpos++] = b;
          total++;
          gz->match_pos++;
        }
      }
      gz->wpos  = wpos;
      gz->total = total;
      if (gz->match_pos == gz->match_len)
        gz->state = S_DECODE;
      break;
    }

    case S_TRAILER: {
      uint32_t pending = gz->wpos - WINDOW_SIZE;
      if (pending > 0)
        gz->crc = crc32_update(gz->crc, gz->window + WINDOW_SIZE, pending);
      br_align(&gz->br);
      uint32_t exp_crc = br_read_u32(&gz->br), exp_size = br_read_u32(&gz->br);
      if ((gz->crc ^ 0xFFFFFFFFu) != exp_crc || gz->total != exp_size) {
        gz->state = S_ERROR;  return -1;
      }
      gz->state = S_DONE;
      break;
    }

    case S_DONE: return (int64_t)written;
    case S_ERROR: return -1;
    default: gz->state = S_ERROR; return -1;
    }
  }

  return (int64_t)written;
}

/*  Limine API wrappers.  */
struct gzip_handle {
  struct file_handle * source;   /*  the compressed file (owned)  */
  gz_state_t         * gz;       /*  decompressor state  */
  uint64_t           dec_pos;    /*  current decompressed stream position  */
};

static void gz_state_init(gz_state_t * gz, struct file_handle * source) {
  memset(gz, 0, sizeof(*gz));
  br_init(&gz->br, source);
  gz->wpos = WINDOW_SIZE;  /*  Semi-space streaming decoding.  */
  gz->crc = 0xFFFFFFFF;
  gz->state = S_HEADER;
}

static void gz_rewind(struct gzip_handle * gh) {
  gz_state_init(gh->gz, gh->source);
  gh->dec_pos = 0;
}

static uint64_t gzip_read(struct file_handle * file, void * buf, uint64_t loc, uint64_t count) {
  struct gzip_handle * gh = file->fd;
  /*  Rewind if the caller seeks backward.  */
  if (loc < gh->dec_pos) { gz_rewind(gh); }
  /*  Skip forward to reach the requested offset. EOS during seek means
      the requested location is past end-of-stream - return 0 bytes.  */
  while (gh->dec_pos < loc) {
    uint8_t discard[4096];
    uint64_t gap = loc - gh->dec_pos;
    size_t chunk = gap > sizeof(discard) ? sizeof(discard) : (size_t)gap;
    int64_t n = gz_decompress(gh->gz, discard, chunk);
    if (n < 0) panic(false, "gzip: decompression error during seek");
    if (n == 0) return 0;
    gh->dec_pos += (uint64_t)n;
  }
  /*  Decompress the requested data.  */
  uint8_t * dst = buf;
  uint64_t remaining = count;
  while (remaining > 0) {
    size_t chunk = remaining > 65536 ? 65536 : (size_t)remaining;
    int64_t n = gz_decompress(gh->gz, dst, chunk);
    if (n < 0)
      panic(false, "gzip: decompression error");
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
  pmm_free(gh->gz, sizeof(gz_state_t));
  pmm_free(gh, sizeof(struct gzip_handle));
}

/*  Public API layer.  */
bool gzip_check(struct file_handle * fd) {
  if (fd->size < 18) return false;
  uint8_t magic[2];  fread(fd, magic, 0, 2);
  return magic[0] == 0x1F && magic[1] == 0x8B;
}

struct file_handle * gzip_open(struct file_handle * compressed) {
  crc32_init_table();
  /*  The decompressed size is not known up front. The 4-byte ISIZE
      trailer is unreliable (modulo 2^32, spec defect) and callers must
      instead drain until gzip_read returns 0 bytes (EOS). Advertise an
      unknown size via UINT64_MAX.  */
  gz_state_t * gz = ext_mem_alloc(sizeof(gz_state_t));
  gz_state_init(gz, compressed);
  struct gzip_handle * gh = ext_mem_alloc(sizeof(struct gzip_handle));
  gh->source = compressed;
  gh->gz = gz;
  gh->dec_pos = 0;
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

/*  The peak memory usage of this (fast) Gzip decoder is estimated
    to be around ~519 KiB per stream. This can be improved by increasing
    Huffman table bits up from 9, decreasing the buffer sizes, etc.
    TABLE_BITS: 9 - 519KiB, 10 - 308KiB, 11 - 212KiB.  */
