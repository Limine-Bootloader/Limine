#ifndef LIB__RAND_H__
#define LIB__RAND_H__

#include <stdint.h>
#include <stddef.h>

/* Obtain hardware (cryptographically secure) entropy, filling up to size
   bytes of buf. Returns the number of bytes actually filled. */
size_t hw_entropy(void *buf, size_t size);

/* Fast, C-like randomness API backed by PCG32. */
void srand(uint32_t s);
uint32_t rand32(void);
uint64_t rand64(void);

/* Hardware-entropy-backed variants for security-sensitive use (e.g. ASLR).
   Fall back to the PRNG above when no hardware source is available. */
uint32_t safe_rand32(void);
uint64_t safe_rand64(void);

#endif
