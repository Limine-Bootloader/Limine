#ifndef DRIVERS__GOP_H
#define DRIVERS__GOP_H

#if defined(UEFI)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/fb.h>

void init_gop(struct fb_info **ret, size_t *_fbs_count,
              uint64_t target_width, uint64_t target_height, uint16_t target_bpp);

extern bool gop_force_16;

#endif

#endif
