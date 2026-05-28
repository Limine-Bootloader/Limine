#include <stdint.h>
#include <stddef.h>
#include <lib/image.h>
#include <lib/config.h>
#include <lib/misc.h>
#include <mm/pmm.h>
#include <lib/qoi.h>
#include <lib/stb_image.h>

void image_make_centered(struct image *image, int frame_x_size, int frame_y_size, uint32_t back_colour) {
    image->type = IMAGE_CENTERED;

    image->x_displacement = (int64_t)frame_x_size / 2 - (int64_t)image->x_size / 2;
    image->y_displacement = (int64_t)frame_y_size / 2 - (int64_t)image->y_size / 2;
    image->back_colour = back_colour;
}


void image_make_stretched(struct image *image, int new_x_size, int new_y_size) {
    image->type = IMAGE_STRETCHED;

    image->x_size = new_x_size;
    image->y_size = new_y_size;
}

static void free_image_data(struct image *image) {
    if (image->isQoi) {
        qoi_free(image->img);
    } else {
        stbi_image_free(image->img);
    }
}

struct image *image_open(struct file_handle *file) {
    struct image *image = ext_mem_alloc(sizeof(struct image));

    image->type = IMAGE_TILED;

    const uint8_t *src = file->fd;

    int x = 0, y = 0;
    image->isQoi = file->size >= 4
                  && src[0] == 'q'
                  && src[1] == 'o'
                  && src[2] == 'i'
                  && src[3] == 'f';

    if (image->isQoi) {
        image->img = qoi_decode(src, file->size, &x, &y);
    } else {
        int bpp;
        image->img = stbi_load_from_memory(src, file->size, &x, &y, &bpp, 4);
    }

    if (image->img == NULL || x == 0 || y == 0) {
        free_image_data(image);
        pmm_free(image, sizeof(struct image));
        return NULL;
    }

    // stb_image returns RGBA bytes (little-endian uint32 ABGR); convert to
    // the framebuffer-native XRGB layout. The QOI decoder already produces
    // XRGB, so this step is skipped on that path.
    if (!image->isQoi) {
        uint32_t *pptr = (void *)image->img;
        size_t pixel_count = CHECKED_MUL((size_t)x, (size_t)y,
            ({ free_image_data(image); pmm_free(image, sizeof(struct image)); return NULL; }));
        for (size_t i = 0; i < pixel_count; i++) {
            pptr[i] = (pptr[i] & 0x0000ff00) | ((pptr[i] & 0x00ff0000) >> 16) | ((pptr[i] & 0x000000ff) << 16);
        }
    }

    image->x_size = x;
    image->y_size = y;
    image->pitch = (int)CHECKED_MUL((size_t)x, 4,
        ({ free_image_data(image); pmm_free(image, sizeof(struct image)); return NULL; }));
    image->bpp = 32;
    image->img_width = x;
    image->img_height = y;

    return image;
}

void image_close(struct image *image) {
    free_image_data(image);
    pmm_free(image, sizeof(struct image));
}
