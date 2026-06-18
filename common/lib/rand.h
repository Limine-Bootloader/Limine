#ifndef LIB__RAND_H__
#define LIB__RAND_H__

#include <stdint.h>
#include <stddef.h>

/* Obtain hardware (cryptographically secure) entropy.
   Can be called from Stage-2 contexts. */
size_t hw_entropy(void *buf, size_t size);

/* Fast, C-like randomness API backed by MT19937. */
void srand(uint32_t s);
uint32_t rand32(void);
uint64_t rand64(void);

#endif
