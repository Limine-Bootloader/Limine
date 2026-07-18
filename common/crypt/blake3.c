// This BLAKE3 implementation is based on the upstream portable implementation.
// https://github.com/BLAKE3-team/BLAKE3

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <crypt/blake3.h>
#include <fs/file.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <mm/pmm.h>

#define BLAKE3_BLOCK_BYTES 64
#define BLAKE3_CHUNK_BYTES 1024
#define BLAKE3_MAX_DEPTH 54

#define CHUNK_START (1 << 0)
#define CHUNK_END   (1 << 1)
#define PARENT      (1 << 2)
#define ROOT        (1 << 3)

static const uint32_t blake3_iv[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

static const uint8_t blake3_msg_schedule[7][16] = {
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    {  2,  6,  3, 10,  7,  0,  4, 13,  1, 11, 12,  5,  9, 14, 15,  8 },
    {  3,  4, 10, 12, 13,  2,  7, 14,  6,  5,  9,  0, 11, 15,  8,  1 },
    { 10,  7, 12,  9, 14,  3, 13, 15,  4,  0, 11,  2,  5,  8,  1,  6 },
    { 12, 13,  9, 11, 15, 10, 14,  8,  7,  2,  5,  3,  0,  1,  6,  4 },
    {  9, 14, 11,  5,  8, 12, 15,  1, 13,  3,  0, 10,  2,  6,  4,  7 },
    { 11, 15,  5,  0,  1,  9,  8,  6, 14, 10,  2, 12,  3,  4,  7, 13 },
};

struct blake3_chunk_state {
    uint32_t cv[8];
    uint64_t chunk_counter;
    uint8_t buf[BLAKE3_BLOCK_BYTES];
    uint8_t buf_len;
    uint8_t blocks_compressed;
};

struct blake3_output {
    uint32_t input_cv[8];
    uint8_t block[BLAKE3_BLOCK_BYTES];
    uint8_t block_len;
    uint64_t counter;
    uint8_t flags;
};

struct blake3_state {
    struct blake3_chunk_state chunk;
    uint8_t cv_stack_len;
    uint8_t cv_stack[(BLAKE3_MAX_DEPTH + 1) * BLAKE3_OUT_BYTES];
};

static inline uint32_t load32_le(const void *src) {
    uint32_t val;
    memcpy(&val, src, sizeof(val));
    return val;
}

static inline void store32_le(void *dst, uint32_t val) {
    memcpy(dst, &val, sizeof(val));
}

static inline uint32_t rotr32(uint32_t w, unsigned c) {
    return (w >> c) | (w << (32 - c));
}

static void g(uint32_t *state, size_t a, size_t b, size_t c, size_t d,
              uint32_t x, uint32_t y) {
    state[a] += state[b] + x;
    state[d] = rotr32(state[d] ^ state[a], 16);
    state[c] += state[d];
    state[b] = rotr32(state[b] ^ state[c], 12);
    state[a] += state[b] + y;
    state[d] = rotr32(state[d] ^ state[a], 8);
    state[c] += state[d];
    state[b] = rotr32(state[b] ^ state[c], 7);
}

static void round_fn(uint32_t state[16], const uint32_t msg[16], size_t round) {
    const uint8_t *schedule = blake3_msg_schedule[round];

    g(state, 0, 4, 8, 12, msg[schedule[0]], msg[schedule[1]]);
    g(state, 1, 5, 9, 13, msg[schedule[2]], msg[schedule[3]]);
    g(state, 2, 6, 10, 14, msg[schedule[4]], msg[schedule[5]]);
    g(state, 3, 7, 11, 15, msg[schedule[6]], msg[schedule[7]]);

    g(state, 0, 5, 10, 15, msg[schedule[8]], msg[schedule[9]]);
    g(state, 1, 6, 11, 12, msg[schedule[10]], msg[schedule[11]]);
    g(state, 2, 7, 8, 13, msg[schedule[12]], msg[schedule[13]]);
    g(state, 3, 4, 9, 14, msg[schedule[14]], msg[schedule[15]]);
}

static void compress_pre(uint32_t state[16], const uint32_t cv[8],
                         const uint8_t block[BLAKE3_BLOCK_BYTES],
                         uint8_t block_len, uint64_t counter, uint8_t flags) {
    uint32_t block_words[16];

    for (size_t i = 0; i < 16; i++) {
        block_words[i] = load32_le(block + i * sizeof(block_words[i]));
    }

    for (size_t i = 0; i < 8; i++) {
        state[i] = cv[i];
        state[i + 8] = blake3_iv[i];
    }
    state[12] = (uint32_t)counter;
    state[13] = (uint32_t)(counter >> 32);
    state[14] = block_len;
    state[15] = flags;

    for (size_t i = 0; i < 7; i++) {
        round_fn(state, block_words, i);
    }
}

static void blake3_compress_in_place(uint32_t cv[8],
                                     const uint8_t block[BLAKE3_BLOCK_BYTES],
                                     uint8_t block_len, uint64_t counter,
                                     uint8_t flags) {
    uint32_t state[16];

    compress_pre(state, cv, block, block_len, counter, flags);

    for (size_t i = 0; i < 8; i++) {
        cv[i] = state[i] ^ state[i + 8];
    }
}

static void blake3_compress_xof(const uint32_t cv[8],
                                const uint8_t block[BLAKE3_BLOCK_BYTES],
                                uint8_t block_len, uint64_t counter,
                                uint8_t flags, uint8_t out[64]) {
    uint32_t state[16];

    compress_pre(state, cv, block, block_len, counter, flags);

    for (size_t i = 0; i < 8; i++) {
        store32_le(out + i * sizeof(uint32_t), state[i] ^ state[i + 8]);
        store32_le(out + (i + 8) * sizeof(uint32_t), state[i + 8] ^ cv[i]);
    }
}

static void store_cv_words(uint8_t out[BLAKE3_OUT_BYTES], const uint32_t cv[8]) {
    for (size_t i = 0; i < 8; i++) {
        store32_le(out + i * sizeof(cv[i]), cv[i]);
    }
}

static void chunk_state_init(struct blake3_chunk_state *state, uint64_t chunk_counter) {
    memcpy(state->cv, blake3_iv, sizeof(state->cv));
    state->chunk_counter = chunk_counter;
    memset(state->buf, 0, sizeof(state->buf));
    state->buf_len = 0;
    state->blocks_compressed = 0;
}

static size_t chunk_state_len(const struct blake3_chunk_state *state) {
    return state->blocks_compressed * BLAKE3_BLOCK_BYTES + state->buf_len;
}

static uint8_t chunk_state_start_flag(const struct blake3_chunk_state *state) {
    return state->blocks_compressed == 0 ? CHUNK_START : 0;
}

static size_t chunk_state_fill_buf(struct blake3_chunk_state *state,
                                   const uint8_t *input, size_t input_len) {
    size_t take = BLAKE3_BLOCK_BYTES - state->buf_len;
    if (take > input_len) {
        take = input_len;
    }
    memcpy(state->buf + state->buf_len, input, take);
    state->buf_len += take;
    return take;
}

static void chunk_state_update(struct blake3_chunk_state *state,
                               const uint8_t *input, size_t input_len) {
    if (state->buf_len != 0) {
        size_t take = chunk_state_fill_buf(state, input, input_len);
        input += take;
        input_len -= take;
        if (input_len != 0) {
            blake3_compress_in_place(state->cv, state->buf,
                                     BLAKE3_BLOCK_BYTES, state->chunk_counter,
                                     chunk_state_start_flag(state));
            state->blocks_compressed++;
            state->buf_len = 0;
            memset(state->buf, 0, sizeof(state->buf));
        }
    }

    while (input_len > BLAKE3_BLOCK_BYTES) {
        blake3_compress_in_place(state->cv, input,
                                 BLAKE3_BLOCK_BYTES, state->chunk_counter,
                                 chunk_state_start_flag(state));
        state->blocks_compressed++;
        input += BLAKE3_BLOCK_BYTES;
        input_len -= BLAKE3_BLOCK_BYTES;
    }

    chunk_state_fill_buf(state, input, input_len);
}

static struct blake3_output chunk_state_output(const struct blake3_chunk_state *state) {
    struct blake3_output output;

    memcpy(output.input_cv, state->cv, sizeof(output.input_cv));
    memcpy(output.block, state->buf, sizeof(output.block));
    output.block_len = state->buf_len;
    output.counter = state->chunk_counter;
    output.flags = chunk_state_start_flag(state) | CHUNK_END;

    return output;
}

static struct blake3_output parent_output(const uint8_t block[BLAKE3_BLOCK_BYTES]) {
    struct blake3_output output;

    memcpy(output.input_cv, blake3_iv, sizeof(output.input_cv));
    memcpy(output.block, block, sizeof(output.block));
    output.block_len = BLAKE3_BLOCK_BYTES;
    output.counter = 0;
    output.flags = PARENT;

    return output;
}

static void output_chaining_value(const struct blake3_output *output,
                                  uint8_t cv[BLAKE3_OUT_BYTES]) {
    uint32_t cv_words[8];

    memcpy(cv_words, output->input_cv, sizeof(cv_words));
    blake3_compress_in_place(cv_words, output->block, output->block_len,
                             output->counter, output->flags);
    store_cv_words(cv, cv_words);
}

static void output_root_bytes(const struct blake3_output *output, void *out,
                              size_t out_len) {
    uint8_t *outp = out;
    uint8_t block[64];

    for (uint64_t counter = 0; out_len != 0; counter++) {
        blake3_compress_xof(output->input_cv, output->block,
                            output->block_len, counter,
                            output->flags | ROOT, block);
        size_t take = out_len < sizeof(block) ? out_len : sizeof(block);
        memcpy(outp, block, take);
        outp += take;
        out_len -= take;
    }

    memset(block, 0, sizeof(block));
}

static void parent_cv(const uint8_t left[BLAKE3_OUT_BYTES],
                      const uint8_t right[BLAKE3_OUT_BYTES],
                      uint8_t out[BLAKE3_OUT_BYTES]) {
    uint8_t block[BLAKE3_BLOCK_BYTES];
    struct blake3_output output;

    memcpy(block, left, BLAKE3_OUT_BYTES);
    memcpy(block + BLAKE3_OUT_BYTES, right, BLAKE3_OUT_BYTES);
    output = parent_output(block);
    output_chaining_value(&output, out);
}

static void push_cv(struct blake3_state *state, const uint8_t cv[BLAKE3_OUT_BYTES]) {
    memcpy(state->cv_stack + state->cv_stack_len * BLAKE3_OUT_BYTES,
           cv, BLAKE3_OUT_BYTES);
    state->cv_stack_len++;
}

static void add_chunk_cv(struct blake3_state *state, uint8_t cv[BLAKE3_OUT_BYTES],
                         uint64_t total_chunks) {
    while ((total_chunks & 1) == 0) {
        uint8_t *left = state->cv_stack + (state->cv_stack_len - 1) * BLAKE3_OUT_BYTES;
        parent_cv(left, cv, cv);
        state->cv_stack_len--;
        total_chunks >>= 1;
    }

    push_cv(state, cv);
}

static void blake3_init(struct blake3_state *state) {
    chunk_state_init(&state->chunk, 0);
    state->cv_stack_len = 0;
}

static void blake3_update(struct blake3_state *state, const void *_input, size_t input_len) {
    const uint8_t *input = _input;

    while (input_len != 0) {
        if (chunk_state_len(&state->chunk) == BLAKE3_CHUNK_BYTES) {
            uint8_t chunk_cv[BLAKE3_OUT_BYTES];
            struct blake3_output output = chunk_state_output(&state->chunk);
            uint64_t total_chunks = state->chunk.chunk_counter + 1;

            output_chaining_value(&output, chunk_cv);
            add_chunk_cv(state, chunk_cv, total_chunks);
            chunk_state_init(&state->chunk, total_chunks);
        }

        size_t want = BLAKE3_CHUNK_BYTES - chunk_state_len(&state->chunk);
        size_t take = input_len < want ? input_len : want;
        chunk_state_update(&state->chunk, input, take);
        input += take;
        input_len -= take;
    }
}

static void blake3_final(struct blake3_state *state, void *out, size_t out_len) {
    struct blake3_output output = chunk_state_output(&state->chunk);

    while (state->cv_stack_len != 0) {
        uint8_t right[BLAKE3_OUT_BYTES];
        uint8_t block[BLAKE3_BLOCK_BYTES];

        output_chaining_value(&output, right);
        state->cv_stack_len--;
        memcpy(block, state->cv_stack + state->cv_stack_len * BLAKE3_OUT_BYTES,
               BLAKE3_OUT_BYTES);
        memcpy(block + BLAKE3_OUT_BYTES, right, BLAKE3_OUT_BYTES);
        output = parent_output(block);
    }

    output_root_bytes(&output, out, out_len);
}

void blake3(void *out, size_t out_len, const void *in, size_t in_len) {
    struct blake3_state state;

    blake3_init(&state);
    blake3_update(&state, in, in_len);
    blake3_final(&state, out, out_len);
}

struct blake3_handle {
    struct file_handle *source;
    struct blake3_state state;
    uint64_t pos;
    bool finalized;
    uint8_t digest[BLAKE3_LONG_OUT_BYTES];
};

static uint64_t blake3_read(struct file_handle *fh, void *buf, uint64_t loc, uint64_t count) {
    struct blake3_handle *h = fh->fd;

    if (loc != h->pos) {
        panic(false, "blake3 filter: non-sequential read (pos=%x, loc=%x)",
              (uint64_t)h->pos, loc);
    }

    uint64_t got = fread(h->source, buf, loc, count);
    blake3_update(&h->state, buf, got);
    h->pos += got;
    return got;
}

static void blake3_close(struct file_handle *fh) {
    struct blake3_handle *h = fh->fd;

    fclose(h->source);
    pmm_free(h, sizeof(struct blake3_handle));
}

struct file_handle *blake3_open(struct file_handle *source) {
    struct blake3_handle *h = ext_mem_alloc(sizeof(struct blake3_handle));
    blake3_init(&h->state);
    h->source = source;
    h->pos = 0;
    h->finalized = false;

    struct file_handle *ret = ext_mem_alloc(sizeof(struct file_handle));
    ret->fd = h;
    ret->read = (void *)blake3_read;
    ret->close = (void *)blake3_close;
    ret->size = source->size;
    ret->vol = source->vol;
    if (source->path != NULL && source->path_len > 0) {
        ret->path = ext_mem_alloc(source->path_len);
        memcpy(ret->path, source->path, source->path_len);
        ret->path_len = source->path_len;
    }
#if defined (UEFI)
    ret->efi_part_handle = source->efi_part_handle;
#endif
    ret->pxe = source->pxe;
    memcpy(ret->pxe_ip, source->pxe_ip, 4);
    ret->pxe_port = source->pxe_port;
    return ret;
}

bool blake3_check_hash(struct file_handle *fh, void *reference_hash, size_t hash_size) {
    if (fh->read != (void *)blake3_read) {
        panic(false, "blake3_check_hash: not a blake3 filter handle");
    }
    if (hash_size > sizeof(((struct blake3_handle *)0)->digest)) {
        panic(false, "blake3_check_hash: digest too large");
    }

    struct blake3_handle *h = fh->fd;
    if (!h->finalized) {
        blake3_final(&h->state, h->digest, sizeof(h->digest));
        h->finalized = true;
    }
    return memcmp(h->digest, reference_hash, hash_size) == 0;
}
