#ifndef CRYPT__BLAKE3_H__
#define CRYPT__BLAKE3_H__

#include <stddef.h>
#include <stdbool.h>

#define BLAKE3_OUT_BYTES 32
#define BLAKE3_LONG_OUT_BYTES 128

void blake3(void *out, size_t out_len, const void *in, size_t in_len);

struct file_handle;
struct file_handle *blake3_open(struct file_handle *source);
bool blake3_check_hash(struct file_handle *fd, void *reference_hash, size_t hash_size);

#endif
