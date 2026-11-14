#ifndef LIB__FB_QUIRKS_H
#define LIB__FB_QUIRKS_H

#include <stddef.h>
#include <lib/fb.h>

// Works out the turn that what is drawn on the first framebuffer needs to look
// upright.
void fb_quirks_apply(struct fb_info *fbs, size_t fbs_count);

#endif
