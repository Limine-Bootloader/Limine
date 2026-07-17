#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <crypt/hash.h>
#include <lib/misc.h>
#include <lib/libc.h>

bool hash_type_from_hex_length(enum hash_type *type, size_t *digest_size, size_t hex_len) {
    switch (hex_len) {
        case HASH_BLAKE3_HEX_CHARS:
            if (type != NULL) {
                *type = HASH_BLAKE3;
            }
            if (digest_size != NULL) {
                *digest_size = BLAKE3_OUT_BYTES;
            }
            return true;
        case HASH_BLAKE2B_HEX_CHARS:
            if (type != NULL) {
                *type = HASH_BLAKE2B;
            }
            if (digest_size != NULL) {
                *digest_size = BLAKE2B_OUT_BYTES;
            }
            return true;
        case HASH_BLAKE3_LONG_HEX_CHARS:
            if (type != NULL) {
                *type = HASH_BLAKE3_LONG;
            }
            if (digest_size != NULL) {
                *digest_size = BLAKE3_LONG_OUT_BYTES;
            }
            return true;
    }

    return false;
}

static bool decode_hex(uint8_t *out, const char *hex, size_t out_size) {
    for (size_t i = 0; i < out_size; i++) {
        int hi = digit_to_int(hex[i * 2]);
        int lo = digit_to_int(hex[i * 2 + 1]);
        if (hi == -1 || lo == -1) {
            return false;
        }
        out[i] = hi << 4 | lo;
    }

    return true;
}

bool hash_reference_from_hex(struct hash_reference *ref, const char *hex, size_t hex_len) {
    if (!hash_type_from_hex_length(&ref->type, &ref->digest_size, hex_len)) {
        return false;
    }

    memset(ref->digest, 0, sizeof(ref->digest));
    return decode_hex(ref->digest, hex, ref->digest_size);
}

bool hash_enrolled_empty(const char *hex) {
    for (size_t i = 0; i < HASH_MAX_HEX_CHARS; i++) {
        if (hex[i] != '0') {
            return false;
        }
    }

    return true;
}

bool hash_reference_from_enrolled(struct hash_reference *ref, const char *hex) {
    if (hash_enrolled_empty(hex)) {
        ref->type = HASH_NONE;
        ref->digest_size = 0;
        memset(ref->digest, 0, sizeof(ref->digest));
        return true;
    }

    // The config slot keeps legacy 128-char BLAKE2b hashes zero-padded in the
    // upper half; any non-zero upper half selects 256-char BLAKE3 XOF.
    for (size_t i = HASH_BLAKE2B_HEX_CHARS; i < HASH_MAX_HEX_CHARS; i++) {
        if (hex[i] != '0') {
            return hash_reference_from_hex(ref, hex, HASH_BLAKE3_LONG_HEX_CHARS);
        }
    }

    return hash_reference_from_hex(ref, hex, HASH_BLAKE2B_HEX_CHARS);
}

const char *hash_type_name(enum hash_type type) {
    switch (type) {
        case HASH_BLAKE2B:
            return "BLAKE2b";
        case HASH_BLAKE3:
        case HASH_BLAKE3_LONG:
            return "BLAKE3";
        default:
            return "unknown";
    }
}

void hash_buffer(enum hash_type type, void *out, size_t out_len, const void *in, size_t in_len) {
    switch (type) {
        case HASH_BLAKE2B:
            blake2b(out, in, in_len);
            break;
        case HASH_BLAKE3:
        case HASH_BLAKE3_LONG:
            blake3(out, out_len, in, in_len);
            break;
        default:
            panic(false, "hash_buffer: invalid hash type");
    }
}

struct file_handle *hash_open(enum hash_type type, struct file_handle *source) {
    switch (type) {
        case HASH_BLAKE2B:
            return blake2b_open(source);
        case HASH_BLAKE3:
        case HASH_BLAKE3_LONG:
            return blake3_open(source);
        default:
            panic(false, "hash_open: invalid hash type");
    }
}

bool hash_check(struct file_handle *fh, const struct hash_reference *ref) {
    switch (ref->type) {
        case HASH_BLAKE2B:
            return blake2b_check_hash(fh, (void *)ref->digest);
        case HASH_BLAKE3:
        case HASH_BLAKE3_LONG:
            return blake3_check_hash(fh, (void *)ref->digest, ref->digest_size);
        default:
            panic(false, "hash_check: invalid hash type");
    }
}
