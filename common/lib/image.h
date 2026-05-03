#ifndef LIB__IMAGE_H__
#define LIB__IMAGE_H__

#include <stdint.h>
#include <fs/file.h>

struct image {
    size_t x_size;
    size_t y_size;
    int type;
    uint8_t *img;
    int bpp;
    int pitch;
    size_t img_width; // x_size = scaled size, img_width = bitmap size
    size_t img_height;
    int64_t x_displacement;
    int64_t y_displacement;
    uint32_t back_colour;
    char isQoi;
};

enum {
    IMAGE_TILED, IMAGE_CENTERED, IMAGE_STRETCHED
};

void image_make_centered(struct image *image, int frame_x_size, int frame_y_size, uint32_t back_colour);
void image_make_stretched(struct image *image, int new_x_size, int new_y_size);
struct image *image_open(struct file_handle *file);
void image_close(struct image *image);

#endif
