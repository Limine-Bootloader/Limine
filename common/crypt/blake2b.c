// This blake2b implementation comes from the GNU coreutils project.
// https://github.com/coreutils/coreutils/blob/master/src/blake2/blake2b-ref.c

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <crypt/blake2b.h>
#include <lib/libc.h>

#define BLAKE2B_BLOCK_BYTES 128
#define BLAKE2B_KEY_BYTES 64
#define BLAKE2B_SALT_BYTES 16
#define BLAKE2B_PERSONAL_BYTES 16

static const uint64_t blake2b_iv[8] = {
    0x6a09e667f3bcc908,
    0xbb67ae8584caa73b,
    0x3c6ef372fe94f82b,
    0xa54ff53a5f1d36f1,
    0x510e527fade682d1,
    0x9b05688c2b3e6c1f,
    0x1f83d9abfb41bd6b,
    0x5be0cd19137e2179,
};

static const uint8_t blake2b_sigma[12][16] = {
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
    { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
    {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
    {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
    {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
    { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
    { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
    {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
    { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
};

struct blake2b_state {
    uint64_t h[8];
    uint64_t t[2];
    uint64_t f[2];
    uint8_t buf[BLAKE2B_BLOCK_BYTES];
    size_t buf_len;
    uint8_t last_node;
};

struct blake2b_param {
    uint8_t digest_length;
    uint8_t key_length;
    uint8_t fan_out;
    uint8_t depth;
    uint32_t leaf_length;
    uint32_t node_offset;
    uint32_t xof_length;
    uint8_t node_depth;
    uint8_t inner_length;
    uint8_t reserved[14];
    uint8_t salt[BLAKE2B_SALT_BYTES];
    uint8_t personal[BLAKE2B_PERSONAL_BYTES];
} __attribute__((packed));

static void blake2b_increment_counter(struct blake2b_state *state, uint64_t inc) {
    state->t[0] += inc;
    state->t[1] += state->t[0] < inc;
}

static inline uint64_t rotr64(uint64_t w, unsigned c) {
    return (w >> c) | (w << (64 - c));
}

#define G(r, i, a, b, c, d) do { \
        a = a + b + m[blake2b_sigma[r][2 * i + 0]]; \
        d = rotr64(d ^ a, 32); \
        c = c + d; \
        b = rotr64(b ^ c, 24); \
        a = a + b + m[blake2b_sigma[r][2 * i + 1]]; \
        d = rotr64(d ^ a, 16); \
        c = c + d; \
        b = rotr64(b ^ c, 63); \
    } while (0)

#define ROUND(r) do { \
        G(r, 0, v[0], v[4], v[8], v[12]); \
        G(r, 1, v[1], v[5], v[9], v[13]); \
        G(r, 2, v[2], v[6], v[10], v[14]); \
        G(r, 3, v[3], v[7], v[11], v[15]); \
        G(r, 4, v[0], v[5], v[10], v[15]); \
        G(r, 5, v[1], v[6], v[11], v[12]); \
        G(r, 6, v[2], v[7], v[8], v[13]); \
        G(r, 7, v[3], v[4], v[9], v[14]); \
    } while (0)

static void blake2b_compress(struct blake2b_state *state, const uint8_t block[static BLAKE2B_BLOCK_BYTES]) {
    uint64_t m[16];
    uint64_t v[16];

    for (int i = 0; i < 16; i++) {
        m[i] = *(uint64_t *)(block + i * sizeof(m[i]));
    }

    for (int i = 0; i < 8; i++) {
        v[i] = state->h[i];
    }

    v[8] = blake2b_iv[0];
    v[9] = blake2b_iv[1];
    v[10] = blake2b_iv[2];
    v[11] = blake2b_iv[3];
    v[12] = blake2b_iv[4] ^ state->t[0];
    v[13] = blake2b_iv[5] ^ state->t[1];
    v[14] = blake2b_iv[6] ^ state->f[0];
    v[15] = blake2b_iv[7] ^ state->f[1];

    ROUND(0);
    ROUND(1);
    ROUND(2);
    ROUND(3);
    ROUND(4);
    ROUND(5);
    ROUND(6);
    ROUND(7);
    ROUND(8);
    ROUND(9);
    ROUND(10);
    ROUND(11);

    for (int i = 0; i < 8; i++) {
        state->h[i] = state->h[i] ^ v[i] ^ v[i + 8];
    }
}

#undef G
#undef ROUND

static void blake2b_init(struct blake2b_state *state) {
    struct blake2b_param param = {0};

    param.digest_length = BLAKE2B_OUT_BYTES;
    param.fan_out = 1;
    param.depth = 1;

    memset(state, 0, sizeof(struct blake2b_state));

    for (int i = 0; i < 8; i++) {
        state->h[i] = blake2b_iv[i];
    }

    for (int i = 0; i < 8; i++) {
        state->h[i] ^= *(uint64_t *)((void *)&param + sizeof(state->h[i]) * i);
    }
}

static void blake2b_update(struct blake2b_state *state, const void *in, size_t in_len) {
    if (in_len == 0) {
        return;
    }

    size_t left = state->buf_len;
    size_t fill = BLAKE2B_BLOCK_BYTES - left;

    if (in_len > fill) {
        state->buf_len = 0;

        memcpy(state->buf + left, in, fill);
        blake2b_increment_counter(state, BLAKE2B_BLOCK_BYTES);
        blake2b_compress(state, state->buf);

        in += fill;
        in_len -= fill;

        while (in_len > BLAKE2B_BLOCK_BYTES) {
            blake2b_increment_counter(state, BLAKE2B_BLOCK_BYTES);
            blake2b_compress(state, in);

            in += BLAKE2B_BLOCK_BYTES;
            in_len -= BLAKE2B_BLOCK_BYTES;
        }
    }

    memcpy(state->buf + state->buf_len, in, in_len);
    state->buf_len += in_len;
}

static void blake2b_final(struct blake2b_state *state, void *out) {
    uint8_t buffer[BLAKE2B_OUT_BYTES] = {0};

    blake2b_increment_counter(state, state->buf_len);
    state->f[0] = (uint64_t)-1;
    memset(state->buf + state->buf_len, 0, BLAKE2B_BLOCK_BYTES - state->buf_len);
    blake2b_compress(state, state->buf);

    for (int i = 0; i < 8; i++) {
        *(uint64_t *)(buffer + sizeof(state->h[i]) * i) = state->h[i];
    }

    memcpy(out, buffer, BLAKE2B_OUT_BYTES);
    memset(buffer, 0, sizeof(buffer));
}

void blake2b(void *out, const void *in, size_t in_len) {
    struct blake2b_state state = {0};

    blake2b_init(&state);
    blake2b_update(&state, in, in_len);
    blake2b_final(&state, out);
}
