/* limlz: Copyright (C) 2026 Kamila Szewczyk <k@iczelia.net>
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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

typedef unsigned char byte;

static uint16_t endswap16(uint16_t value) {
    uint16_t ret = 0;
    ret |= (value >> 8) & 0x00ff;
    ret |= (value << 8) & 0xff00;
    return ret;
}

static uint32_t endswap32(uint32_t value) {
    uint32_t ret = 0;
    ret |= (value >> 24) & 0x000000ff;
    ret |= (value >> 8)  & 0x0000ff00;
    ret |= (value << 8)  & 0x00ff0000;
    ret |= (value << 24) & 0xff000000;
    return ret;
}

static uint64_t endswap64(uint64_t value) {
    uint64_t ret = 0;
    ret |= (value >> 56) & 0x00000000000000ff;
    ret |= (value >> 40) & 0x000000000000ff00;
    ret |= (value >> 24) & 0x0000000000ff0000;
    ret |= (value >> 8)  & 0x00000000ff000000;
    ret |= (value << 8)  & 0x000000ff00000000;
    ret |= (value << 24) & 0x0000ff0000000000;
    ret |= (value << 40) & 0x00ff000000000000;
    ret |= (value << 56) & 0xff00000000000000;
    return ret;
}

#ifdef __BYTE_ORDER__

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define bigendian true
#else
#define bigendian false
#endif

#else /* !__BYTE_ORDER__ */

static bool bigendian = false;

#endif /* !__BYTE_ORDER__ */

#define ENDSWAP(VALUE) (bigendian ? (                    \
    sizeof(VALUE) == 1 ? (VALUE)          :              \
    sizeof(VALUE) == 2 ? endswap16(VALUE) :              \
    sizeof(VALUE) == 4 ? endswap32(VALUE) :              \
    sizeof(VALUE) == 8 ? endswap64(VALUE) : (abort(), 1) \
) : (VALUE))

/*  Higher -> better compression with exponentally dimnishing gains.  */
#define LIMLZ_SA_NEIGHBORS 32

struct sa_cmp_ctx { int * rank; size_t n, k; };
static struct sa_cmp_ctx g_sa_ctx;

static int sa_cmp_idx(int i, int j) {
  int ri, rj;
  if (g_sa_ctx.rank[i] != g_sa_ctx.rank[j])
    return g_sa_ctx.rank[i] - g_sa_ctx.rank[j];
  ri = (i + (int)g_sa_ctx.k < (int)g_sa_ctx.n) ? g_sa_ctx.rank[i + g_sa_ctx.k] : -1;
  rj = (j + (int)g_sa_ctx.k < (int)g_sa_ctx.n) ? g_sa_ctx.rank[j + g_sa_ctx.k] : -1;
  return ri - rj;
}

static int sa_qsort_cmp(const void * a, const void * b) {
  int i = *(const int *) a, j = *(const int *) b;
  return sa_cmp_idx(i, j);
}

static int saca(const byte * s, size_t n, int * sa, int * rank, int * tmp) {
  size_t i;
  if (!n)
    return 0;
  for (i = 0; i < n; ++i) {
    sa[i] = (int)i;  rank[i] = (int)s[i];
  }
  for (g_sa_ctx.k = 1;; g_sa_ctx.k <<= 1) {
    g_sa_ctx.rank = rank;  g_sa_ctx.n = n;
    qsort(sa, n, sizeof(sa[0]), sa_qsort_cmp);
    tmp[sa[0]] = 0;
    for (i = 1; i < n; ++i)
      tmp[sa[i]] = tmp[sa[i - 1]] + (sa_cmp_idx(sa[i - 1], sa[i]) < 0);
    for (i = 0; i < n; ++i)
      rank[i] = tmp[i];
    if ((size_t)rank[sa[n - 1]] == n - 1)
      break;
  }
  return 0;
}

static size_t lcp_bytes(const byte * s, size_t n, size_t i, size_t j) {
  size_t l = 0, m = n - (i > j ? i : j);
  for (; l < m && s[i + l] == s[j + l]; ++l);
  return l;
}

struct match_choice { uint32_t len;  uint16_t off; };
struct parse_choice { uint32_t lit, mlen;  uint16_t off; };

static int longest_matches(const byte * src, size_t n, struct match_choice * mch) {
  int * sa, * rank, * tmp, * inv;
  size_t i;
  if (!n)
    return 0;
  sa = malloc(n * sizeof(*sa));
  rank = malloc(n * sizeof(*rank));
  tmp = malloc(n * sizeof(*tmp));
  inv = malloc(n * sizeof(*inv));
  if (!sa || !rank || !tmp || !inv || saca(src, n, sa, rank, tmp)) {
    free(sa);  free(rank);  free(tmp);  free(inv);
    return -1;
  }
  for (i = 0; i < n; ++i)
    inv[sa[i]] = (int)i;
  for (i = 0; i < n; ++i) {
    int r = inv[i], d, rr;
    size_t best_len = 0;
    uint16_t best_off = 0;
    for (d = -LIMLZ_SA_NEIGHBORS; d <= LIMLZ_SA_NEIGHBORS; ++d) {
      size_t j, l, off;
      if (!d)
        continue;
      rr = r + d;
      if (rr < 0 || rr >= (int)n)
        continue;
      j = (size_t)sa[rr];
      if (j >= i)
        continue;
      off = i - j;
      if (off == 0 || off > 65535)
        continue;
      l = lcp_bytes(src, n, i, j);
      if (l > best_len) {
        best_len = l;
        best_off = (uint16_t)off;
      }
    }
    if (best_len >= 4) {
      mch[i].len = (uint32_t)best_len;
      mch[i].off = best_off;
    } else {
      mch[i].len = mch[i].off = 0;
    }
  }
  free(sa);  free(rank);  free(tmp);  free(inv);
  return 0;
}

static int encode_len_tail(byte ** outp, byte * out_end, size_t n) {
  byte *out = * outp;
  if (n >= 15) {
    if (out >= out_end) return -1;
    *out++ = (byte)(n - 15);
  }
  *outp = out;
  return 0;
}

static int encode_len_tail_ml(byte ** outp, byte * out_end, size_t n) {
  byte * out = *outp;
  if (n >= 7) {
    if (out >= out_end)
      return -1;
    *out++ = (byte)(n - 7);
  }
  *outp = out;
  return 0;
}

static size_t limlzpack(void * dst, size_t dstcap, const void * srcv, size_t srcsz) {
  const byte * src = (const byte *) srcv;
  byte * dstp = (byte *) dst;
  byte * out = dstp, * out_end = dstp + dstcap;
  struct match_choice * mch, * bestm;
  struct parse_choice * pick;
  size_t i, * dp;
  if (!srcsz) {
    if (dstcap < 1)
      return 0;
    dstp[0] = 0;
    return 1;
  }
  mch = calloc(srcsz, sizeof(*mch));
  pick = calloc(srcsz + 1, sizeof(*pick));
  bestm = calloc(srcsz, sizeof(*bestm));
  dp = malloc((srcsz + 1) * sizeof(*dp));
  if (!mch || !pick || !bestm || !dp || longest_matches(src, srcsz, mch))
    goto fail;
  dp[srcsz] = 0;
  pick[srcsz].lit = pick[srcsz].mlen = pick[srcsz].off = 0;
  for (i = srcsz; i-- > 0;) {
    size_t j, best_cost;
    uint32_t best_lit, best_len;
    uint16_t best_off;
    bestm[i].len = bestm[i].off = 0;
    if (mch[i].len >= 4) {
      size_t ml, lim = mch[i].len;
      if (lim > 266) lim = 266;
      size_t off_bytes = (mch[i].off > 255) ? 2 : 1;
      size_t mcost = (size_t)-1;
      uint32_t mlen = 0;
      if (i + lim > srcsz)
        lim = srcsz - i;
      for (ml = 4; ml <= lim; ++ml) {
        size_t c = off_bytes + (ml - 4 >= 7) + dp[i + ml];
        if (c < mcost) {
          mcost = c;  mlen = (uint32_t)ml;
        }
      }
      if (mlen) {
        bestm[i].len = mlen;  bestm[i].off = mch[i].off;
      }
    }
    if (srcsz - i <= 270) { // 256 + 15 - 1
      best_cost = 1 + (srcsz - i) + (srcsz - i >= 15);
      best_lit = (uint32_t)(srcsz - i);
    } else {
      best_cost = (size_t)-1;
      best_lit = 0;
    }
    best_len = best_off = 0;
    for (j = i; j < srcsz && j - i <= 270; ++j) {
      size_t lit = j - i, off_bytes_j, c;
      if (!bestm[j].len)
        continue;
      off_bytes_j = (bestm[j].off > 255) ? 2 : 1;
      c = 1 + lit + (lit >= 15) +
                  (off_bytes_j + (bestm[j].len - 4 >= 7) + dp[j + bestm[j].len]);
      if (c < best_cost) {
        best_cost = c;  best_lit = (uint32_t)lit;
        best_len = bestm[j].len;  best_off = bestm[j].off;
      }
    }
    dp[i] = best_cost;  pick[i].lit = best_lit;
    pick[i].mlen = best_len;  pick[i].off = best_off;
  }
  int terminated = 0;
  for (i = 0; i < srcsz; ) {
    byte * tokenp;
    size_t lit = pick[i].lit, ml = pick[i].mlen;
    uint16_t off = pick[i].off;
    unsigned token_hi, token_lo;
    if (i + lit > srcsz)
      goto fail;
    if (i + lit < srcsz && ml < 4)
      goto fail;
    tokenp = out;
    if (out >= out_end)
      goto fail;
    *out++ = 0;
    token_hi = (lit < 15) ? (unsigned)lit : 15u;
    if (encode_len_tail(&out, out_end, lit))
      goto fail;
    if ((size_t)(out_end - out) < lit)
      goto fail;
    memcpy(out, src + i, lit);
    out += lit;
    i += lit;
    if (i >= srcsz) {
      *tokenp = (byte)(token_hi << 3);
      terminated = 1;
      break;
    }
    unsigned mode_bit = (off > 255) ? 1u : 0u;
    token_lo = (ml - 4 < 7) ? (unsigned)(ml - 4) : 7u;
    *tokenp = (byte)((mode_bit << 7) | (token_hi << 3) | token_lo);
    if (off > 255) {
      if (out_end - out < 2)
        goto fail;
      *out++ = (byte)(off & 255);
      *out++ = (byte)(off >> 8);
    } else {
      if (out >= out_end)
        goto fail;
      *out++ = (byte)off;
    }
    if (encode_len_tail_ml(&out, out_end, ml - 4))
      goto fail;
    i += ml;
  }
  /* A match-ended parse leaves no trailing token; the decompressor keys
   * termination off a zero-or-more-byte literal copy reaching ipe, so always
   * emit a final lit=0 token when the main loop didn't already. */
  if (!terminated) {
    if (out >= out_end)
      goto fail;
    *out++ = 0;
  }
  free(mch);  free(pick);  free(bestm);  free(dp);
  return (size_t)(out - dstp);
fail:
  free(mch);  free(pick);  free(bestm);  free(dp);
  return 0;
}

static const uint32_t tab[16] = {
  0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
  0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
  0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
  0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
};

static uint32_t crc32_nibble(const byte *data, size_t len) {
  uint32_t crc = ~0u; // faster than decompressor.asm bit-by-bit, same result.
  while (len--) {
    crc ^= *data++;
    crc = (crc >> 4) ^ tab[crc & 0x0Fu];
    crc = (crc >> 4) ^ tab[crc & 0x0Fu];
  }
  return ~crc;
}

int main(int argc, char *argv[]) {
#ifndef __BYTE_ORDER__
  uint32_t endcheck = 0x12345678;
  uint8_t endbyte = *((uint8_t *)&endcheck);
  bigendian = endbyte == 0x12;
#endif
  if (argc != 3) {
    fprintf(stderr, "? %s <input> <output>\n", argv[0]);  return 1;
  }
  FILE * fin = fopen(argv[1], "rb");
  FILE * fout = fopen(argv[2], "wb");
  byte * inbuf, *outbuf;
  size_t insz, outsz;
  if (!fin || !fout) {
    fprintf(stderr, "? fopen\n");  return 1;
  }
  fseek(fin, 0, SEEK_END);
  insz = ftell(fin);
  fseek(fin, 0, SEEK_SET);
  inbuf = malloc(insz);  outbuf = malloc(insz * 2);
  if (!inbuf || !outbuf) {
    fprintf(stderr, "? malloc\n");  return 1;
  }
  fread(inbuf, 1, insz, fin);
  fclose(fin);
  outsz = limlzpack(outbuf, insz * 2, inbuf, insz);
  if (!outsz) {
    fprintf(stderr, "? limlzpack\n");  return 1;
  }
  uint32_t crc = ENDSWAP(crc32_nibble(inbuf, insz));
  fwrite(&crc, sizeof(crc), 1, fout);
  fwrite(outbuf, 1, outsz, fout);
  fclose(fout);
  free(inbuf);  free(outbuf);
}
