/* limlz: Copyright (C) 2026 Kamila Szewczyk <k@iczelia.net>
 * limine: Copyright (C) 2019-2026 Mintsuki and contributors.
 *
 * The algorithm is based on LZMA, augmented with a x86 filter and a
 * Storer-Szymanski backwards optimal parse.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#define LIMLZ_HEADER_SIZE 8
#define LIMLZ_PROBS 4918
#define LIMLZ_IS_REP 192
#define LIMLZ_REP_G0 204
#define LIMLZ_REP_G1 216
#define LIMLZ_REP_G2 228
#define LIMLZ_REP_LONG 240
#define LIMLZ_SLOT 432
#define LIMLZ_SPECIAL 688
#define LIMLZ_ALIGN 802
#define LIMLZ_LENGTH 818
#define LIMLZ_REP_LENGTH 1332
#define LIMLZ_LITERAL 1846
#define LIMLZ_MAX_MATCH 273

#define HASH_SIZE 65536u
#define CHAIN_LIMIT 128

static inline uint32_t limlz_read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8
        | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint32_t limlz_crc32(const uint8_t *data, size_t size) {
    static const uint32_t table[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
    };
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ table[crc & 15];
        crc = (crc >> 4) ^ table[crc & 15];
    }
    return ~crc;
}

struct range_encoder {
    uint8_t *data;
    size_t size;
    size_t capacity;
    uint64_t low;
    uint32_t range;
    size_t pending;
    uint8_t cache;
    int error;
    uint16_t probs[LIMLZ_PROBS];
};

struct match {
    unsigned length;
    uint32_t distance;
};

struct match_finder {
    const uint8_t *data;
    uint32_t size;
    uint32_t *previous;
    uint32_t head[HASH_SIZE];
};

static void put_byte(struct range_encoder *rc, uint8_t value) {
    if (rc->error) {
        return;
    }
    if (rc->size == rc->capacity) {
        if (rc->capacity > SIZE_MAX / 2) {
            rc->error = 1;
            return;
        }
        size_t capacity = rc->capacity ? rc->capacity * 2 : 4096;
        uint8_t *data = realloc(rc->data, capacity);
        if (!data) {
            rc->error = 1;
            return;
        }
        rc->data = data;
        rc->capacity = capacity;
    }
    rc->data[rc->size++] = value;
}

static void put32(struct range_encoder *rc, uint32_t value) {
    for (unsigned i = 0; i < 32; i += 8) {
        put_byte(rc, (uint8_t)(value >> i));
    }
}

static void shift_low(struct range_encoder *rc) {
    uint32_t low = (uint32_t)rc->low;
    unsigned carry = (unsigned)(rc->low >> 32);
    /* Delay bytes whose value still depends on a carry from the next interval. */
    if (low < 0xff000000u || carry) {
        uint8_t value = rc->cache;
        do {
            put_byte(rc, (uint8_t)(value + carry));
            value = 255;
        } while (--rc->pending);
        rc->cache = (uint8_t)(low >> 24);
    }
    rc->pending++;
    rc->low = (uint32_t)(low << 8);
}

static void normalise(struct range_encoder *rc) {
    if (rc->range < UINT32_C(0x1000000)) {
        rc->range <<= 8;
        shift_low(rc);
    }
}

static void encode_bit(struct range_encoder *rc, unsigned index, unsigned bit) {
    uint16_t *prob = rc->probs + index;
    uint32_t bound = (rc->range >> 11) * *prob;
    if (!bit) {
        rc->range = bound;
        *prob = (uint16_t)(*prob + ((2048 - *prob) >> 5));
    } else {
        rc->low += bound;
        rc->range -= bound;
        *prob = (uint16_t)(*prob - (*prob >> 5));
    }
    normalise(rc);
}

static void encode_tree(struct range_encoder *rc, unsigned index, unsigned bits, unsigned value) {
    unsigned node = 1;
    while (bits) {
        unsigned bit = (value >> --bits) & 1;
        encode_bit(rc, index + node, bit);
        node = (node << 1) | bit;
    }
}

static void encode_reverse(struct range_encoder *rc, unsigned index, unsigned bits, unsigned value) {
    unsigned node = 1;
    for (unsigned i = 0; i < bits; i++) {
        unsigned bit = (value >> i) & 1;
        encode_bit(rc, index + node, bit);
        node = (node << 1) | bit;
    }
}

static void encode_length(struct range_encoder *rc, unsigned index, unsigned pos, unsigned length) {
    encode_bit(rc, index, length >= 10);
    if (length < 10) {
        encode_tree(rc, index + 2 + pos * 8, 3, length - 2);
    } else {
        encode_bit(rc, index + 1, length >= 18);
        if (length < 18) {
            encode_tree(rc, index + 130 + pos * 8, 3, length - 10);
        } else {
            encode_tree(rc, index + 258, 8, length - 18);
        }
    }
}

static void encode_distance(struct range_encoder *rc, unsigned length, uint32_t distance) {
    uint32_t value = distance - 1;
    unsigned slot = value;
    unsigned bits = 0;
    if (value >= 4) {
        uint32_t high = value;
        while (high >= 4) {
            high >>= 1;
            bits++;
        }
        slot = (bits + 1) * 2 + (high & 1);
    }
    unsigned len_state = length < 6 ? length - 2 : 3;
    encode_tree(rc, LIMLZ_SLOT + len_state * 64, 6, slot);
    if (slot < 4) {
        return;
    }
    uint32_t base = (uint32_t)(2 | (slot & 1)) << bits;
    uint32_t extra = value - base;
    if (slot < 14) {
        encode_reverse(rc, LIMLZ_SPECIAL + base - slot - 1, bits, extra);
    } else {
        for (unsigned i = bits; i > 4; i--) {
            rc->range >>= 1;
            if ((extra >> (i - 1)) & 1) {
                rc->low += rc->range;
            }
            normalise(rc);
        }
        encode_reverse(rc, LIMLZ_ALIGN, 4, extra & 15);
    }
}

static unsigned hash_at(const uint8_t *data) {
    return (unsigned)data[0] | (unsigned)data[1] << 8;
}

static void insert(struct match_finder *mf, uint32_t pos) {
    if (mf->size - pos >= 2) {
        unsigned hash = hash_at(mf->data + pos);
        mf->previous[pos] = mf->head[hash];
        mf->head[hash] = pos + 1;
    }
}

static unsigned match_length(const struct match_finder *mf, uint32_t pos, uint32_t distance) {
    uint32_t limit = mf->size - pos;
    if (limit > LIMLZ_MAX_MATCH) {
        limit = LIMLZ_MAX_MATCH;
    }
    unsigned length = 0;
    while (length < limit && mf->data[pos + length] == mf->data[pos - distance + length]) {
        length++;
    }
    return length;
}

static unsigned distance_price(uint32_t distance) {
    unsigned bits = 0;
    for (uint32_t value = distance - 1; value >= 4; value >>= 1) {
        bits++;
    }
    return 6 + bits;
}

static int parse(struct match_finder *mf, struct match *choices) {
    uint64_t *cost = malloc(((size_t)mf->size + 1) * sizeof(*cost));
    if (!cost) {
        return 0;
    }
    for (uint32_t pos = 0; pos < mf->size; pos++) {
        insert(mf, pos);
    }
    cost[mf->size] = 0;
    for (uint32_t pos = mf->size; pos-- > 0;) {
        cost[pos] = 9 + cost[pos + 1];
        choices[pos] = (struct match){1, 0};
        if (mf->size - pos < 2) {
            continue;
        }
        uint32_t candidate = mf->previous[pos];
        unsigned longest = 1;
        for (unsigned depth = 0; candidate && depth < CHAIN_LIMIT; depth++) {
            uint32_t previous = candidate - 1;
            candidate = mf->previous[previous];
            if (longest >= mf->size - pos || longest == LIMLZ_MAX_MATCH) {
                break;
            }
            if (mf->data[pos + longest] != mf->data[previous + longest]) {
                continue;
            }
            uint32_t distance = pos - previous;
            unsigned length = match_length(mf, pos, distance);
            unsigned price = 2 + distance_price(distance);
            /* Older candidates cost at least as much for lengths already covered. */
            for (unsigned len = longest + 1; len <= length; len++) {
                unsigned length_price = len < 10 ? 4 : len < 18 ? 5 : 10;
                uint64_t next_cost = price + length_price + cost[pos + len];
                if (next_cost <= cost[pos]) {
                    cost[pos] = next_cost;
                    choices[pos] = (struct match){len, distance};
                }
            }
            if (length > longest) {
                longest = length;
            }
        }
    }
    free(cost);
    return 1;
}

static void encode_literal(struct range_encoder *rc, const uint8_t *data, uint32_t pos,
                           unsigned state, uint32_t distance) {
    unsigned previous = pos ? data[pos - 1] : 0;
    unsigned index = LIMLZ_LITERAL + (previous >> 6) * 768;
    unsigned node = 1;
    unsigned offset = state >= 7 ? 256 : 0;
    unsigned match = offset ? data[pos - distance] : 0;
    for (unsigned i = 8; i > 0; i--) {
        unsigned bit = (data[pos] >> (i - 1)) & 1;
        match <<= 1;
        encode_bit(rc, index + node + offset + (match & offset), bit);
        node = (node << 1) | bit;
        if (bit != ((match >> 8) & 1)) {
            offset = 0;
        }
    }
}

static void bcj_encode(uint8_t *data, uint32_t size) {
    uint32_t previous = UINT32_MAX - 4;
    unsigned history = 0;
    for (uint32_t pos = 0; size - pos >= 5;) {
        if (data[pos] != 0xe8 && data[pos] != 0xe9) {
            pos++;
            continue;
        }
        uint32_t gap = pos - previous;
        previous = pos;
        if (gap >= 4) {
            history = 0;
        } else {
            while (gap--) {
                history = (history & 0x77) << 1;
            }
        }
        unsigned high = data[pos + 4];
        if ((high == 0 || high == 255) && history <= 8 && history != 6) {
            uint32_t value = limlz_read32(data + pos + 1);
            unsigned shift = 0;
            if (history) {
                unsigned bit = 0;
                for (unsigned mask = history; mask >>= 1;) {
                    bit++;
                }
                shift = 24 - bit * 8;
            }
            for (;;) {
                value += pos + 5;
                high = (value >> shift) & 255;
                if (!history || (high != 0 && high != 255)) {
                    break;
                }
                value ^= ~(0xFFFFFF00u << shift);
            }
            /* BCJ canonicalises the high byte from bit 24. */
            data[pos + 4] = (uint8_t)(0u - ((value >> 24) & 1));
            for (unsigned i = 0; i < 3; i++) {
                data[pos + 1 + i] = (uint8_t)(value >> (i * 8));
            }
            pos += 5;
            history = 0;
        } else {
            history |= 1;
            if (high == 0 || high == 255) {
                history |= 16;
            }
            pos++;
        }
    }
}

static int compress(struct range_encoder *rc, uint8_t *data, uint32_t size) {
    if ((uint64_t)size + 1 > SIZE_MAX / sizeof(uint64_t)
        || (uint64_t)size + 1 > SIZE_MAX / sizeof(struct match)
        || (uint64_t)size + 1 > SIZE_MAX / sizeof(uint32_t)) {
        return 0;
    }
    uint32_t checksum = limlz_crc32(data, size);
    bcj_encode(data, size);
    struct match_finder *mf = calloc(1, sizeof(*mf));
    if (!mf) {
        return 0;
    }
    mf->data = data;
    mf->size = size;
    size_t count = size ? size : 1;
    mf->previous = malloc(count * sizeof(*mf->previous));
    struct match *choices = malloc(count * sizeof(*choices));
    if (!mf->previous || !choices || !parse(mf, choices)) {
        free(choices);
        free(mf->previous);
        free(mf);
        return 0;
    }
    rc->range = UINT32_MAX;
    rc->pending = 1;
    for (unsigned i = 0; i < LIMLZ_PROBS; i++) {
        rc->probs[i] = 1024;
    }
    put32(rc, checksum);
    put32(rc, size);
    unsigned state = 0;
    uint32_t reps[4] = {1, 1, 1, 1};
    uint32_t pos = 0;
    while (pos < size && !rc->error) {
        struct match match = choices[pos];
        unsigned match_rep = 4;
        for (unsigned i = 0; i < 4; i++) {
            if (match.distance == reps[i]) {
                match_rep = i;
                break;
            }
        }
        unsigned context = state * 16;
        if (match.length < 2) {
            if (reps[0] <= pos && data[pos] == data[pos - reps[0]]) {
                encode_bit(rc, context, 1);
                encode_bit(rc, LIMLZ_IS_REP + state, 1);
                encode_bit(rc, LIMLZ_REP_G0 + state, 0);
                encode_bit(rc, LIMLZ_REP_LONG + context, 0);
                state = state < 7 ? 9 : 11;
            } else {
                encode_bit(rc, context, 0);
                encode_literal(rc, data, pos, state, reps[0]);
                state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            }
            pos++;
            continue;
        }
        encode_bit(rc, context, 1);
        encode_bit(rc, LIMLZ_IS_REP + state, match_rep < 4);
        if (match_rep < 4) {
            encode_bit(rc, LIMLZ_REP_G0 + state, match_rep != 0);
            if (!match_rep) {
                encode_bit(rc, LIMLZ_REP_LONG + context, 1);
            } else {
                encode_bit(rc, LIMLZ_REP_G1 + state, match_rep != 1);
                if (match_rep >= 2) {
                    encode_bit(rc, LIMLZ_REP_G2 + state, match_rep == 3);
                }
            }
            encode_length(rc, LIMLZ_REP_LENGTH, 0, match.length);
            state = state < 7 ? 8 : 11;
        } else {
            encode_length(rc, LIMLZ_LENGTH, 0, match.length);
            encode_distance(rc, match.length, match.distance);
            state = state < 7 ? 7 : 10;
        }
        unsigned rep = match_rep < 4 ? match_rep : 3;
        for (unsigned i = rep; i > 0; i--) {
            reps[i] = reps[i - 1];
        }
        reps[0] = match.distance;
        pos += match.length;
    }
    for (unsigned i = 0; i < 5; i++) {
        shift_low(rc);
    }
    free(choices);
    free(mf->previous);
    free(mf);
    return !rc->error;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input> <output>\n", argv[0]);
        return 1;
    }
    int result = 1;
    FILE *input = fopen(argv[1], "rb");
    FILE *output = NULL;
    uint8_t *data = NULL;
    struct range_encoder rc = {0};
    if (!input) {
        perror(argv[1]);
        goto done;
    }
    size_t limit = SIZE_MAX < UINT32_MAX ? SIZE_MAX : UINT32_MAX;
    size_t capacity = limit < 65536 ? limit : 65536;
    size_t size = 0;
    data = malloc(capacity);
    if (!data) {
        fprintf(stderr, "limlzpack: allocation failed\n");
        goto done;
    }
    for (;;) {
        size += fread(data + size, 1, capacity - size, input);
        if (size < capacity) {
            break;
        }
        int byte = fgetc(input);
        if (byte == EOF) {
            break;
        }
        if (size == limit) {
            fprintf(stderr, "limlzpack: input exceeds format or host size limit\n");
            goto done;
        }
        capacity = capacity > limit / 2 ? limit : capacity * 2;
        uint8_t *grown = realloc(data, capacity);
        if (!grown) {
            fprintf(stderr, "limlzpack: allocation failed\n");
            goto done;
        }
        data = grown;
        data[size++] = (uint8_t)byte;
    }
    if (ferror(input)) {
        fprintf(stderr, "limlzpack: cannot read input\n");
        goto done;
    }
    if (fclose(input)) {
        input = NULL;
        fprintf(stderr, "limlzpack: cannot close input\n");
        goto done;
    }
    input = NULL;
    if (!compress(&rc, data, (uint32_t)size)) {
        fprintf(stderr, "limlzpack: allocation failed\n");
        goto done;
    }
    output = fopen(argv[2], "wb");
    if (!output) {
        perror(argv[2]);
        goto done;
    }
    if (fwrite(rc.data, 1, rc.size, output) != rc.size) {
        fprintf(stderr, "limlzpack: cannot write output\n");
        goto done;
    }
    if (fclose(output)) {
        output = NULL;
        fprintf(stderr, "limlzpack: cannot close output\n");
        goto done;
    }
    output = NULL;
    result = 0;
done:
    if (input) {
        fclose(input);
    }
    if (output) {
        fclose(output);
    }
    free(data);
    free(rc.data);
    return result;
}
