#ifndef LIB__UTF_H
#define LIB__UTF_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// src ends after src_len code units or at a NUL, whichever comes first. dest
// holds dest_size code units and is NUL-terminated unless dest_size is 0.
// Ill-formed input becomes U+FFFD, and conversion stops at the first code
// point that does not fit whole. The number of code units stored, NUL
// excluded, goes to *dest_len unless dest_len is NULL. Both return whether all
// of src was converted.
bool utf16_to_utf8(char *dest, size_t dest_size, const uint16_t *src, size_t src_len, size_t *dest_len);
bool utf8_to_utf16(uint16_t *dest, size_t dest_size, const char *src, size_t src_len, size_t *dest_len);

#endif
