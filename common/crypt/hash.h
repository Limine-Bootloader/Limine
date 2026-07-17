#ifndef CRYPT__HASH_H__
#define CRYPT__HASH_H__

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <crypt/blake2b.h>
#include <crypt/blake3.h>

#define HASH_BLAKE3_HEX_CHARS (BLAKE3_OUT_BYTES * 2)
#define HASH_BLAKE2B_HEX_CHARS (BLAKE2B_OUT_BYTES * 2)
#define HASH_BLAKE3_LONG_HEX_CHARS (BLAKE3_LONG_OUT_BYTES * 2)
#define HASH_MAX_HEX_CHARS HASH_BLAKE3_LONG_HEX_CHARS
#define HASH_MAX_BYTES BLAKE3_LONG_OUT_BYTES

enum hash_type {
    HASH_NONE,
    HASH_BLAKE2B,
    HASH_BLAKE3,
    HASH_BLAKE3_LONG,
};

struct hash_reference {
    enum hash_type type;
    size_t digest_size;
    uint8_t digest[HASH_MAX_BYTES];
};

bool hash_type_from_hex_length(enum hash_type *type, size_t *digest_size, size_t hex_len);
bool hash_reference_from_hex(struct hash_reference *ref, const char *hex, size_t hex_len);
bool hash_reference_from_enrolled(struct hash_reference *ref, const char *hex);
bool hash_enrolled_empty(const char *hex);
const char *hash_type_name(enum hash_type type);
void hash_buffer(enum hash_type type, void *out, size_t out_len, const void *in, size_t in_len);

struct file_handle;
struct file_handle *hash_open(enum hash_type type, struct file_handle *source);
bool hash_check(struct file_handle *fh, const struct hash_reference *ref);

#endif
