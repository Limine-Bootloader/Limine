#ifndef CRYPT__HASH_H
#define CRYPT__HASH_H

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

#define HASH_NONE 0
#define HASH_BLAKE2B 1
#define HASH_BLAKE3 2
#define HASH_BLAKE3_LONG 3

struct hash_reference {
    int type;
    size_t digest_size;
    uint8_t digest[HASH_MAX_BYTES];
};

bool hash_type_from_hex_length(int *type, size_t *digest_size, size_t hex_len);
bool hash_reference_from_hex(struct hash_reference *ref, const char *hex, size_t hex_len);
bool hash_reference_from_enrolled(struct hash_reference *ref, const char *hex);
bool hash_enrolled_empty(const char *hex);
const char *hash_type_name(int type);
void hash_buffer(int type, void *out, size_t out_len, const void *in, size_t in_len);

struct file_handle;
struct file_handle *hash_open(int type, struct file_handle *source);
bool hash_check(struct file_handle *fh, const struct hash_reference *ref);

#endif
