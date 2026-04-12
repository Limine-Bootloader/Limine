#ifndef CRYPT__BLAKE2B_H__
#define CRYPT__BLAKE2B_H__

#include <stddef.h>

#define BLAKE2B_OUT_BYTES 64

void blake2b(void *out, const void *in, size_t in_len);

struct file_handle;
struct file_handle * blake2b_open(struct file_handle * source);
bool blake2b_check_hash(struct file_handle *fd, void* reference_hash);

#endif
