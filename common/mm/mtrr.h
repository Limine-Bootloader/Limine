#ifndef MM__MTRR_H
#define MM__MTRR_H

#include <stdint.h>
#include <stdbool.h>

#if defined(__x86_64__) || defined(__i386__)

void mtrr_save(void);
void mtrr_restore(void);

bool mtrr_wc_add_fb_range(uint64_t base, uint64_t size);

#endif

#endif
