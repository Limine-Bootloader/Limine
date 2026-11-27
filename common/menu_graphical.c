#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <lib/config.h>
#include <lib/fb.h>
#include <lib/gterm.h>
#include <lib/image.h>
#include <lib/misc.h>
#include <lib/term.h>
#include <lib/uri.h>
#include <menu_graphical.h>
#include <mm/pmm.h>

#define MENU_GRAPHICS_MAX_PIXELS ((size_t)4096 * 2160)
#define MENU_GRAPHICS_MAX_ICONS 64
#define MENU_GRAPHICS_MAX_ICON_DIMENSION 256
#define MENU_GRAPHICS_MAX_FONT_SIZE ((size_t)2 * 1024 * 1024)
#define MENU_GRAPHICS_MAX_FONT_GLYPHS 1024
#define MENU_GRAPHICS_FONT_HEADER_SIZE 24
#define MENU_GRAPHICS_FONT_GLYPH_SIZE 20

struct menu_graphics {
    struct fb_info *fb;
    uint32_t *base;
    uint32_t *canvas;
    size_t width;
    size_t height;
    size_t buffer_size;
    size_t row_height;
    size_t row_gap;
    size_t row_start;
    size_t row_count;
    size_t visible_rows;
    size_t item_x;
    size_t item_y;
    size_t item_width;
    size_t cell_width;
    size_t cell_height;
    int rotation;
    const uint8_t *font;
    size_t font_height;
    size_t font_scale_x;
    size_t font_scale_y;
    size_t font_spacing;
    uint8_t *font_data;
    size_t font_data_size;
    size_t font_glyph_count;
    size_t font_data_offset;
    size_t font_line_height;
    size_t font_ascent;
    size_t font_max_advance;
    uint32_t panel_colour;
    uint32_t text_colour;
    uint32_t muted_colour;
    uint32_t accent_colour;
    struct image *header_image;
    bool header_has_alpha;
    struct menu_icon *icons;
    size_t icon_count;
    size_t icon_allocation_size;
    bool active;
};

struct menu_icon {
    char *value;
    char *path;
    size_t name_size;
    size_t value_size;
    struct image *image;
    bool has_alpha;
};

static struct menu_graphics graphics;

static size_t min_size(size_t a, size_t b) {
    return a < b ? a : b;
}

static size_t max_size(size_t a, size_t b) {
    return a > b ? a : b;
}

static uint16_t read_u16(const uint8_t *data) {
    return data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32(const uint8_t *data) {
    return data[0] | ((uint32_t)data[1] << 8)
         | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static int16_t read_i16(const uint8_t *data) {
    return (int16_t)read_u16(data);
}

static bool load_menu_font(void) {
    char *path = config_get_value(NULL, 0, "INTERFACE_MENU_FONT");
    if (path == NULL || (secure_boot_active && strchr(path, '#') == NULL)) {
        return false;
    }

    struct file_handle *file = uri_open_optional(path,
        MEMMAP_BOOTLOADER_RECLAIMABLE,
#if defined(__i386__)
        false, NULL, NULL
#else
        true
#endif
    );
    if (file == NULL || file->size < MENU_GRAPHICS_FONT_HEADER_SIZE
     || file->size > MENU_GRAPHICS_MAX_FONT_SIZE) {
        if (file != NULL) {
            fclose(file);
        }
        return false;
    }

    uint8_t *data = ext_mem_alloc(file->size);
    size_t file_size = file->size;
    bool read_ok = fread(file, data, 0, file_size) == file_size;
    fclose(file);
    if (!read_ok || memcmp(data, "LBF1", 4) != 0
     || read_u16(data + 4) != 1
     || read_u16(data + 14) != 0) {
        pmm_free(data, file_size);
        return false;
    }

    size_t line_height = read_u16(data + 6);
    size_t ascent = read_u16(data + 8);
    size_t max_advance = read_u16(data + 10);
    size_t glyph_count = read_u16(data + 12);
    size_t glyphs_size;
    if (line_height == 0 || line_height > 128 || ascent > line_height
     || max_advance == 0 || max_advance > 256 || glyph_count == 0
     || glyph_count > MENU_GRAPHICS_MAX_FONT_GLYPHS
     || __builtin_mul_overflow(glyph_count, MENU_GRAPHICS_FONT_GLYPH_SIZE,
                               &glyphs_size)
     || MENU_GRAPHICS_FONT_HEADER_SIZE + glyphs_size > file_size) {
        pmm_free(data, file_size);
        return false;
    }

    size_t data_offset = read_u32(data + 16);
    size_t data_size = read_u32(data + 20);
    if (data_offset < MENU_GRAPHICS_FONT_HEADER_SIZE + glyphs_size
     || data_offset > file_size || data_size > file_size - data_offset
     || data_offset + data_size != file_size) {
        pmm_free(data, file_size);
        return false;
    }

    const uint8_t *records = data + MENU_GRAPHICS_FONT_HEADER_SIZE;
    uint32_t previous_codepoint = 0;
    bool has_fallback = false;
    for (size_t i = 0; i < glyph_count; i++) {
        const uint8_t *glyph = records + i * MENU_GRAPHICS_FONT_GLYPH_SIZE;
        uint32_t codepoint = read_u32(glyph);
        size_t advance = read_u16(glyph + 4);
        size_t width = read_u16(glyph + 10);
        size_t height = read_u16(glyph + 12);
        size_t offset = read_u32(glyph + 16);
        size_t bitmap_size;
        if ((i != 0 && codepoint <= previous_codepoint)
         || codepoint > 0x10ffff
         || (codepoint >= 0xd800 && codepoint <= 0xdfff)
         || advance > max_advance
         || width > 128 || height > 128
         || read_u16(glyph + 14) != 0
         || __builtin_mul_overflow(width, height, &bitmap_size)
         || offset > data_size || bitmap_size > data_size - offset) {
            pmm_free(data, file_size);
            return false;
        }
        has_fallback |= codepoint == '?';
        previous_codepoint = codepoint;
    }
    if (!has_fallback) {
        pmm_free(data, file_size);
        return false;
    }

    graphics.font_data = data;
    graphics.font_data_size = file_size;
    graphics.font_glyph_count = glyph_count;
    graphics.font_data_offset = data_offset;
    graphics.font_line_height = line_height;
    graphics.font_ascent = ascent;
    graphics.font_max_advance = max_advance;
    return true;
}

static uint32_t parse_colour(const char *key, uint32_t fallback) {
    char *value = config_get_value(NULL, 0, key);
    if (value == NULL || strlen(value) != 6) {
        return fallback;
    }
    for (size_t i = 0; i < 6; i++) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
           || (c >= 'A' && c <= 'F'))) {
            return fallback;
        }
    }
    return (uint32_t)strtoui(value, NULL, 16);
}

static void load_icons(void) {
    size_t option_count = 0;
    while (option_count < MENU_GRAPHICS_MAX_ICONS
        && config_get_value(NULL, option_count, "INTERFACE_MENU_ICON") != NULL) {
        option_count++;
    }
    if (option_count == 0) {
        return;
    }

    graphics.icon_allocation_size = option_count * sizeof(*graphics.icons);
    graphics.icons = ext_mem_alloc(graphics.icon_allocation_size);
    for (size_t i = 0; i < option_count; i++) {
        char *option = config_get_value(NULL, i, "INTERFACE_MENU_ICON");
        size_t option_size = strlen(option) + 1;
        char *value = ext_mem_alloc(option_size);
        memcpy(value, option, option_size);
        char *separator = strchr(value, '=');
        if (separator == NULL || separator == value || separator[1] == '\0') {
            pmm_free(value, option_size);
            continue;
        }

        struct menu_icon *icon = &graphics.icons[graphics.icon_count];
        icon->value = value;
        icon->value_size = option_size;
        icon->path = separator + 1;
        icon->name_size = separator - value;
        *separator = '\0';
        graphics.icon_count++;
        if (secure_boot_active && strchr(icon->path, '#') == NULL) {
            continue;
        }

        struct file_handle *file = uri_open_optional(icon->path,
            MEMMAP_BOOTLOADER_RECLAIMABLE,
#if defined(__i386__)
            false, NULL, NULL
#else
            true
#endif
        );
        if (file == NULL) {
            continue;
        }
        const uint8_t *file_data = file->fd;
        icon->has_alpha = file->size >= 8
            && file_data[0] == 0x89 && file_data[1] == 'P'
            && file_data[2] == 'N' && file_data[3] == 'G'
            && file_data[4] == '\r' && file_data[5] == '\n'
            && file_data[6] == 0x1a && file_data[7] == '\n';
        icon->image = image_open(file);
        fclose(file);
        if (icon->image != NULL
         && (icon->image->img_width > MENU_GRAPHICS_MAX_ICON_DIMENSION
          || icon->image->img_height > MENU_GRAPHICS_MAX_ICON_DIMENSION)) {
            image_close(icon->image);
            icon->image = NULL;
        }
    }
}

static void load_header(void) {
    char *path = config_get_value(NULL, 0, "INTERFACE_MENU_HEADER");
    if (path == NULL) {
        return;
    }

    struct file_handle *file = uri_open_optional(path,
        MEMMAP_BOOTLOADER_RECLAIMABLE,
#if defined(__i386__)
        false, NULL, NULL
#else
        true
#endif
    );
    if (file == NULL) {
        return;
    }

    const uint8_t *file_data = file->fd;
    graphics.header_has_alpha = file->size >= 8
        && file_data[0] == 0x89 && file_data[1] == 'P'
        && file_data[2] == 'N' && file_data[3] == 'G'
        && file_data[4] == '\r' && file_data[5] == '\n'
        && file_data[6] == 0x1a && file_data[7] == '\n';
    graphics.header_image = image_open(file);
    fclose(file);
    if (graphics.header_image != NULL
     && (graphics.header_image->img_width > 2048
      || graphics.header_image->img_height > 1024
      || graphics.header_image->img_width
       > ((size_t)2 * 1024 * 1024) / graphics.header_image->img_height)) {
        image_close(graphics.header_image);
        graphics.header_image = NULL;
    }
}

static struct menu_icon *find_icon(const char *name) {
    size_t name_size = strlen(name);
    for (size_t i = 0; i < graphics.icon_count; i++) {
        struct menu_icon *icon = &graphics.icons[i];
        if (icon->name_size == name_size
         && memcmp(icon->value, name, name_size) == 0) {
            return icon;
        }
    }
    return NULL;
}

static void physical_position(size_t x, size_t y, size_t *px, size_t *py) {
    switch (graphics.rotation) {
        case 1: {
            *px = graphics.fb->framebuffer_width - 1 - y;
            *py = x;
            break;
        }
        case 2: {
            *px = graphics.fb->framebuffer_width - 1 - x;
            *py = graphics.fb->framebuffer_height - 1 - y;
            break;
        }
        case 3: {
            *px = y;
            *py = graphics.fb->framebuffer_height - 1 - x;
            break;
        }
        default: {
            *px = x;
            *py = y;
            break;
        }
    }
}

static uint8_t pixel_channel(uint32_t pixel, uint8_t size, uint8_t shift) {
    if (size == 0) {
        return 0;
    }
    uint32_t value = (pixel >> shift) & (((uint32_t)1 << size) - 1);
    return (uint8_t)((value * 255) / (((uint32_t)1 << size) - 1));
}

static uint32_t framebuffer_pixel(size_t x, size_t y) {
    size_t px, py;
    physical_position(x, y, &px, &py);
    volatile uint32_t *framebuffer = (void *)(uintptr_t)graphics.fb->framebuffer_addr;
    uint32_t pixel = framebuffer[py * (graphics.fb->framebuffer_pitch / 4) + px];
    uint32_t red = pixel_channel(pixel, graphics.fb->red_mask_size,
                                 graphics.fb->red_mask_shift);
    uint32_t green = pixel_channel(pixel, graphics.fb->green_mask_size,
                                   graphics.fb->green_mask_shift);
    uint32_t blue = pixel_channel(pixel, graphics.fb->blue_mask_size,
                                  graphics.fb->blue_mask_shift);
    return (red << 16) | (green << 8) | blue;
}

static uint32_t framebuffer_colour(uint32_t colour) {
    uint32_t red = (colour >> 16) & 0xff;
    uint32_t green = (colour >> 8) & 0xff;
    uint32_t blue = colour & 0xff;
    red = (red * (((uint32_t)1 << graphics.fb->red_mask_size) - 1) + 127) / 255;
    green = (green * (((uint32_t)1 << graphics.fb->green_mask_size) - 1) + 127) / 255;
    blue = (blue * (((uint32_t)1 << graphics.fb->blue_mask_size) - 1) + 127) / 255;
    return (red << graphics.fb->red_mask_shift)
         | (green << graphics.fb->green_mask_shift)
         | (blue << graphics.fb->blue_mask_shift);
}

static uint32_t blend(uint32_t foreground, uint32_t background, uint8_t alpha) {
    uint32_t inverse = 255 - alpha;
    uint32_t red = (((foreground >> 16) & 0xff) * alpha
                  + ((background >> 16) & 0xff) * inverse + 127) / 255;
    uint32_t green = (((foreground >> 8) & 0xff) * alpha
                    + ((background >> 8) & 0xff) * inverse + 127) / 255;
    uint32_t blue = ((foreground & 0xff) * alpha
                   + (background & 0xff) * inverse + 127) / 255;
    return (red << 16) | (green << 8) | blue;
}

static void put_pixel(int x, int y, uint32_t colour, uint8_t alpha) {
    if (x < 0 || y < 0 || (size_t)x >= graphics.width
     || (size_t)y >= graphics.height) {
        return;
    }
    uint32_t *pixel = &graphics.canvas[(size_t)y * graphics.width + (size_t)x];
    *pixel = alpha == 255 ? colour : blend(colour, *pixel, alpha);
}

static void fill_rect(int x, int y, int width, int height,
                      uint32_t colour, uint8_t alpha) {
    for (int dy = 0; dy < height; dy++) {
        for (int dx = 0; dx < width; dx++) {
            put_pixel(x + dx, y + dy, colour, alpha);
        }
    }
}

static void draw_glyph(uint32_t glyph, int x, int y, uint32_t colour,
                       int max_x) {
    if (glyph > 255) {
        glyph = '?';
    }
    const uint8_t *bits = graphics.font + glyph * graphics.font_height;
    for (size_t row = 0; row < graphics.font_height; row++) {
        for (size_t column = 0; column < 8; column++) {
            if ((bits[row] & (0x80 >> column)) == 0) {
                continue;
            }
            for (size_t sy = 0; sy < graphics.font_scale_y; sy++) {
                for (size_t sx = 0; sx < graphics.font_scale_x; sx++) {
                    int px = x + column * graphics.font_scale_x + sx;
                    int py = y + row * graphics.font_scale_y + sy;
                    if (px < max_x) {
                        put_pixel(px, py, colour, 255);
                    }
                }
            }
        }
    }
}

static size_t utf8_next(const unsigned char **text) {
    const unsigned char *p = *text;
    uint32_t codepoint;
    size_t length;
    if (p[0] < 0x80) {
        codepoint = p[0];
        length = 1;
    } else if ((p[0] & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
        codepoint = ((p[0] & 0x1f) << 6) | (p[1] & 0x3f);
        length = 2;
    } else if ((p[0] & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80
            && (p[2] & 0xc0) == 0x80) {
        codepoint = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6)
                  | (p[2] & 0x3f);
        length = 3;
    } else if ((p[0] & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80
            && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
        codepoint = ((p[0] & 0x07) << 18) | ((p[1] & 0x3f) << 12)
                  | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f);
        length = 4;
    } else {
        codepoint = '?';
        length = 1;
    }
    *text += length;
    return codepoint;
}

static const uint8_t *find_menu_glyph(uint32_t codepoint) {
    size_t low = 0;
    size_t high = graphics.font_glyph_count;
    const uint8_t *records = graphics.font_data + MENU_GRAPHICS_FONT_HEADER_SIZE;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const uint8_t *glyph = records + middle * MENU_GRAPHICS_FONT_GLYPH_SIZE;
        uint32_t current = read_u32(glyph);
        if (current == codepoint) {
            return glyph;
        }
        if (current < codepoint) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return NULL;
}

static const uint8_t *menu_glyph(uint32_t codepoint) {
    const uint8_t *glyph = find_menu_glyph(codepoint);
    if (glyph == NULL && codepoint != '?') {
        glyph = find_menu_glyph('?');
    }
    return glyph;
}

static size_t text_width(const char *text) {
    size_t width = 0;
    if (graphics.font_data == NULL) {
        return strlen(text) * graphics.cell_width;
    }

    for (const unsigned char *p = (const unsigned char *)text; *p != 0; ) {
        uint32_t codepoint = utf8_next(&p);
        const uint8_t *glyph = menu_glyph(codepoint);
        if (glyph != NULL) {
            width += read_u16(glyph + 4) + graphics.font_spacing;
        }
    }
    return width;
}

static void draw_menu_glyph(const uint8_t *glyph, int x, int baseline,
                            uint32_t colour, int max_x) {
    int x_offset = read_i16(glyph + 6);
    int y_offset = read_i16(glyph + 8);
    size_t width = read_u16(glyph + 10);
    size_t height = read_u16(glyph + 12);
    size_t bitmap_offset = read_u32(glyph + 16);
    const uint8_t *bitmap = graphics.font_data + graphics.font_data_offset
                          + bitmap_offset;
    int origin_x = x + x_offset;
    int origin_y = baseline + y_offset;

    for (size_t y = 0; y < height; y++) {
        for (size_t x_pixel = 0; x_pixel < width; x_pixel++) {
            uint8_t alpha = bitmap[y * width + x_pixel];
            int pixel_x = origin_x + (int)x_pixel;
            int pixel_y = origin_y + (int)y;
            if (alpha != 0 && pixel_x < max_x) {
                put_pixel(pixel_x, pixel_y, colour, alpha);
            }
        }
    }
}

static void draw_text(const char *text, int x, int y, int max_x,
                      uint32_t colour) {
    if (graphics.font_data != NULL) {
        int baseline = y + graphics.font_ascent;
        for (const unsigned char *p = (const unsigned char *)text;
             *p != 0; ) {
            uint32_t codepoint = utf8_next(&p);
            const uint8_t *glyph = menu_glyph(codepoint);
            if (glyph == NULL) {
                continue;
            }
            int advance = read_u16(glyph + 4) + graphics.font_spacing;
            if (x + advance > max_x) {
                break;
            }
            draw_menu_glyph(glyph, x, baseline, colour, max_x);
            x += advance;
        }
        return;
    }

    int advance = 8 * graphics.font_scale_x + graphics.font_spacing;
    for (const unsigned char *p = (const unsigned char *)text; *p != 0; ) {
        uint32_t glyph = utf8_next(&p);
        if (x + advance > max_x) {
            break;
        }
        draw_glyph(glyph, x, y, colour, max_x);
        x += advance;
    }
}

static void draw_border(int x, int y, int width, int height, uint32_t colour) {
    fill_rect(x, y, width, 1, colour, 255);
    fill_rect(x, y + height - 1, width, 1, colour, 255);
    fill_rect(x, y, 1, height, colour, 255);
    fill_rect(x + width - 1, y, 1, height, colour, 255);
}

static void draw_image(struct image *image, bool has_alpha,
                       int x, int y, size_t box_width, size_t box_height) {
    size_t width = box_width;
    size_t height = box_height;
    if (image->img_width * height > image->img_height * width) {
        height = image->img_height * width / image->img_width;
    } else {
        width = image->img_width * height / image->img_height;
    }
    if (width == 0) {
        width = 1;
    }
    if (height == 0) {
        height = 1;
    }
    int origin_x = x + (box_width - width) / 2;
    int origin_y = y + (box_height - height) / 2;
    for (size_t dy = 0; dy < height; dy++) {
        size_t source_y = dy * image->img_height / height;
        for (size_t dx = 0; dx < width; dx++) {
            size_t source_x = dx * image->img_width / width;
            uint32_t pixel = ((uint32_t *)image->img)[source_y * image->img_width
                                                     + source_x];
            uint8_t alpha = has_alpha ? 255 - (pixel >> 24) : 255;
            put_pixel(origin_x + dx, origin_y + dy, pixel & 0xffffff, alpha);
        }
    }
}

static void draw_diamond(int x, int y, uint32_t colour) {
    for (int offset = -4; offset <= 4; offset++) {
        int width = 4 - (offset < 0 ? -offset : offset);
        fill_rect(x - width, y + offset, width * 2 + 1, 1, colour, 255);
    }
}

static void draw_help(void) {
    struct menu_hint {
        const char *key;
        const char *label;
    };
    static const struct menu_hint hints[] = {
        { "ARROWS", "Select" },
        { "ENTER", "Boot" },
        { "E", "Edit" },
        { "B", "Blank entry" },
        { "S", "Firmware" },
    };

    size_t key_padding = 12;
    size_t group_gap = 18;
    size_t total_width = 0;
    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        total_width += text_width(hints[i].key) + key_padding + 10
                     + text_width(hints[i].label);
        if (i != 0) {
            total_width += group_gap;
        }
    }
    if (total_width >= graphics.width) {
        return;
    }
    int x = (graphics.width - total_width) / 2;
    int y = graphics.height - graphics.cell_height - 40;
    size_t key_height = graphics.cell_height + 8;

    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        size_t key_width = text_width(hints[i].key) + key_padding;
        fill_rect(x, y, key_width, key_height, 0x332c22, 170);
        draw_border(x, y, key_width, key_height, 0x8c7154);
        draw_text(hints[i].key, x + (int)(key_padding / 2),
                  y + 4, x + (int)key_width, 0xf5dbb8);
        x += key_width + 10;
        draw_text(hints[i].label, x, y + 4,
                  x + text_width(hints[i].label),
                  graphics.muted_colour);
        x += text_width(hints[i].label) + group_gap;
    }
}

bool menu_graphical_init(void) {
    char *style = config_get_value(NULL, 0, "INTERFACE_MENU_STYLE");
    if (style == NULL || strcmp(style, "graphical") != 0
     || term_backend != GTERM || SERIAL_CONSOLE || fb_fbs_count == 0) {
        return false;
    }

    for (size_t i = 0; i < fb_fbs_count; i++) {
        struct fb_info *fb = &fb_fbs[i];
        if (fb->framebuffer_bpp == 32 && fb->framebuffer_addr != 0
         && fb->framebuffer_width != 0 && fb->framebuffer_height != 0
         && fb->framebuffer_pitch >= fb->framebuffer_width * 4) {
            graphics.fb = fb;
            break;
        }
    }
    if (graphics.fb == NULL) {
        return false;
    }

    if (!gterm_get_menu_font(&graphics.font, &graphics.font_height,
                             &graphics.font_scale_x, &graphics.font_scale_y,
                             &graphics.font_spacing)) {
        graphics.fb = NULL;
        return false;
    }
    graphics.rotation = gterm_get_rotation(NULL, graphics.fb);
    if (graphics.font_height == 0 || graphics.font_scale_x == 0
     || graphics.font_scale_y == 0 || graphics.rotation < 0
     || graphics.rotation > 3) {
        graphics.fb = NULL;
        graphics.font = NULL;
        return false;
    }

    if (graphics.rotation == 1 || graphics.rotation == 3) {
        graphics.width = graphics.fb->framebuffer_height;
        graphics.height = graphics.fb->framebuffer_width;
    } else {
        graphics.width = graphics.fb->framebuffer_width;
        graphics.height = graphics.fb->framebuffer_height;
    }
    if (graphics.width < 320 || graphics.height < 180
     || graphics.width > 4096 || graphics.height > 4096
     || graphics.width > MENU_GRAPHICS_MAX_PIXELS / graphics.height) {
        graphics.fb = NULL;
        graphics.font = NULL;
        return false;
    }

    graphics.buffer_size = graphics.width * graphics.height * sizeof(uint32_t);
    graphics.base = ext_mem_alloc(graphics.buffer_size);
    graphics.canvas = ext_mem_alloc(graphics.buffer_size);
    for (size_t y = 0; y < graphics.height; y++) {
        for (size_t x = 0; x < graphics.width; x++) {
            graphics.base[y * graphics.width + x] = framebuffer_pixel(x, y);
        }
    }

    graphics.cell_width = 8 * graphics.font_scale_x + graphics.font_spacing;
    graphics.cell_height = graphics.font_height * graphics.font_scale_y;
    if (load_menu_font()) {
        graphics.cell_width = graphics.font_max_advance + graphics.font_spacing;
        graphics.cell_height = graphics.font_line_height;
    }
    graphics.panel_colour = parse_colour("INTERFACE_MENU_PANEL_COLOUR", 0x111923);
    graphics.text_colour = parse_colour("INTERFACE_MENU_TEXT_COLOUR", 0xe8edf2);
    graphics.muted_colour = parse_colour("INTERFACE_MENU_MUTED_COLOUR", 0x9aaec2);
    graphics.accent_colour = parse_colour("INTERFACE_MENU_ACCENT_COLOUR", 0x52d9ff);
    load_header();
    load_icons();
    graphics.active = true;
    return true;
}

void menu_graphical_suspend(void) {
    graphics.active = false;
}

void menu_graphical_resume(void) {
    if (graphics.fb != NULL && graphics.base != NULL && graphics.canvas != NULL) {
        graphics.active = true;
    }
}

static void draw_backdrop(void) {
    memcpy(graphics.canvas, graphics.base, graphics.buffer_size);
    graphics.item_x = graphics.width * 11 / 100;
    graphics.item_y = graphics.height * 41 / 100;
    graphics.item_width = graphics.width * 34 / 100;
    graphics.row_height = min_size(72,
        max_size(48, max_size(graphics.height / 14, graphics.cell_height + 20)));
    graphics.row_gap = max_size(8, graphics.height / 90);
}

static void present(void) {
    volatile uint32_t *framebuffer = (void *)(uintptr_t)graphics.fb->framebuffer_addr;
    size_t pitch = graphics.fb->framebuffer_pitch / sizeof(uint32_t);
    for (size_t y = 0; y < graphics.height; y++) {
        for (size_t x = 0; x < graphics.width; x++) {
            size_t px, py;
            physical_position(x, y, &px, &py);
            framebuffer[py * pitch + px] =
                framebuffer_colour(graphics.canvas[y * graphics.width + x]);
        }
    }
    fb_flush_cb((volatile void *)framebuffer,
                (size_t)graphics.fb->framebuffer_pitch * graphics.fb->framebuffer_height);
}

void menu_graphical_draw(const struct menu_model *model, size_t selected,
                         const char *branding, bool hide_help,
                         const char *status) {
    if (!graphics.active) {
        return;
    }
    draw_backdrop();

    size_t footer_height = hide_help ? 0 : graphics.cell_height + 24;
    size_t available = graphics.height > graphics.item_y + footer_height + 24
        ? graphics.height - graphics.item_y - footer_height - 24 : 1;
    graphics.visible_rows = min_size(model->count, available
        / (graphics.row_height + graphics.row_gap));
    if (graphics.visible_rows == 0 && model->count != 0) {
        graphics.visible_rows = 1;
    }
    if (selected < graphics.row_start) {
        graphics.row_start = selected;
    } else if (selected >= graphics.row_start + graphics.visible_rows
            && graphics.visible_rows != 0) {
        graphics.row_start = selected - graphics.visible_rows + 1;
    }
    if (graphics.row_start + graphics.visible_rows > model->count) {
        graphics.row_start = model->count > graphics.visible_rows
            ? model->count - graphics.visible_rows : 0;
    }
    graphics.row_count = model->count;
    size_t header_width = min_size(graphics.width * 38 / 100, 720);
    size_t header_height = min_size(graphics.height * 34 / 100, 360);
    int header_x = graphics.width * 12 / 100;
    int header_y = graphics.height * 4 / 100;
    if (graphics.header_image != NULL) {
        draw_image(graphics.header_image, graphics.header_has_alpha,
                   header_x, header_y, header_width, header_height);
    } else if (branding != NULL && branding[0] != '\0') {
        draw_text(branding, header_x, header_y + (int)graphics.cell_height,
                  header_x + (int)header_width, graphics.accent_colour);
    }

    for (size_t visible = 0; visible < graphics.visible_rows; visible++) {
        size_t index = graphics.row_start + visible;
        const struct menu_entry *entry = model->rows[index].entry;
        int y = graphics.item_y + (int)(visible * (graphics.row_height + graphics.row_gap));
        bool is_selected = index == selected;
        if (is_selected) {
            fill_rect(graphics.item_x, y, graphics.item_width,
                      graphics.row_height, graphics.panel_colour, 104);
            draw_border(graphics.item_x, y, graphics.item_width,
                        graphics.row_height, graphics.accent_colour);
            draw_diamond(graphics.item_x - 10,
                         y + (int)(graphics.row_height / 2),
                         graphics.accent_colour);
        }

        size_t icon_size = min_size(graphics.row_height - 12, 32);
        int icon_x = graphics.item_x + 40;
        int icon_y = y + (graphics.row_height - icon_size) / 2;
        struct menu_icon *icon = find_icon(entry->name);
        if (icon != NULL && icon->image != NULL) {
            draw_image(icon->image, icon->has_alpha,
                       icon_x, icon_y, icon_size, icon_size);
        } else {
            char initial[2] = { entry->name[0], '\0' };
            if (initial[0] >= 'a' && initial[0] <= 'z') {
                initial[0] -= 'a' - 'A';
            }
            draw_text(initial, icon_x + (int)(icon_size - graphics.cell_width) / 2,
                      icon_y + (int)(icon_size - graphics.cell_height) / 2,
                      icon_x + (int)icon_size, graphics.accent_colour);
        }

        int label_x = icon_x + (int)icon_size + 18;
        draw_text(entry->name, label_x,
                  y + ((int)graphics.row_height - (int)graphics.cell_height) / 2,
                  graphics.item_x + graphics.item_width - 16,
                  is_selected ? graphics.text_colour : graphics.muted_colour);
        if (entry->sub != NULL) {
            draw_text(">",
                      graphics.item_x + graphics.item_width - 2 * graphics.cell_width,
                      y + ((int)graphics.row_height - (int)graphics.cell_height) / 2,
                      graphics.item_x + graphics.item_width - 8,
                      graphics.muted_colour);
        }
    }

    if (!hide_help && status != NULL) {
        size_t status_width = text_width(status);
        draw_text(status, (graphics.width - status_width) / 2,
                  graphics.height - (int)footer_height,
                  graphics.width - (graphics.width - status_width) / 2,
                  graphics.muted_colour);
    } else if (!hide_help) {
        draw_help();
    }
    present();
}

bool menu_graphical_hit_test(size_t cell_x, size_t cell_y, size_t *index) {
    if (!graphics.active || graphics.cell_width == 0 || graphics.cell_height == 0) {
        return false;
    }
    size_t x, y;
    if (!gterm_menu_mouse_to_pixel(cell_x, cell_y, &x, &y)) {
        return false;
    }
    if (x < graphics.item_x || x >= graphics.item_x + graphics.item_width
     || y < graphics.item_y || graphics.row_height == 0) {
        return false;
    }
    size_t row = (y - graphics.item_y) / (graphics.row_height + graphics.row_gap);
    if (row >= graphics.visible_rows
     || (y - graphics.item_y) % (graphics.row_height + graphics.row_gap)
        >= graphics.row_height) {
        return false;
    }
    *index = graphics.row_start + row;
    return *index < graphics.row_count;
}

void menu_graphical_close(void) {
    if (graphics.base != NULL) {
        pmm_free(graphics.base, graphics.buffer_size);
    }
    if (graphics.canvas != NULL) {
        pmm_free(graphics.canvas, graphics.buffer_size);
    }
    if (graphics.header_image != NULL) {
        image_close(graphics.header_image);
    }
    if (graphics.font_data != NULL) {
        pmm_free(graphics.font_data, graphics.font_data_size);
    }
    for (size_t i = 0; i < graphics.icon_count; i++) {
        if (graphics.icons[i].image != NULL) {
            image_close(graphics.icons[i].image);
        }
        pmm_free(graphics.icons[i].value, graphics.icons[i].value_size);
    }
    if (graphics.icons != NULL) {
        pmm_free(graphics.icons, graphics.icon_allocation_size);
    }
    memset(&graphics, 0, sizeof(graphics));
}
