#ifndef LIB__GTERM_H__
#define LIB__GTERM_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <lib/fb.h>

bool gterm_init(struct fb_info **ret, size_t *_fbs_count,
                char *config, size_t width, size_t height);

int gterm_get_rotation(char *config);

struct flanterm_params {
    uint32_t *canvas;
    size_t canvas_size;
    uint32_t ansi_colours[8];
    uint32_t ansi_bright_colours[8];
    uint32_t default_bg;
    uint32_t default_fg;
    uint32_t default_bg_bright;
    uint32_t default_fg_bright;
    uint8_t *font;
    size_t font_width;
    size_t font_height;
    size_t font_spacing;
    size_t font_scale_x;
    size_t font_scale_y;
    size_t margin;
    int rotation;
};

size_t gterm_prepare_flanterm_params(struct fb_info *fbs, size_t fbs_count,
                                     struct flanterm_params *out, size_t out_max);

#endif
