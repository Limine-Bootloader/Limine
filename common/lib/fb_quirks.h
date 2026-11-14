#ifndef LIB__FB_QUIRKS_H
#define LIB__FB_QUIRKS_H

#include <stddef.h>
#include <lib/fb.h>

// Corrects what the firmware is known to misreport of the first framebuffer
// and of each of its modes, and works out the turn that what is drawn on it
// needs to look upright.
void fb_quirks_apply(struct fb_info *fbs, size_t fbs_count);

#endif
