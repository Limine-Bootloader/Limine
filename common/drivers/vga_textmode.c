#if defined (BIOS)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <drivers/vga_textmode.h>
#include <sys/cpu.h>
#include <lib/real.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/term.h>
#include <mm/pmm.h>

#define VIDEO_BOTTOM ((VD_ROWS * VD_COLS) - 1)

static void draw_cursor(struct textmode_context *ctx) {
    uint8_t pal = ctx->back_buffer[ctx->cursor_offset + 1];
    ctx->video_mem[ctx->cursor_offset + 1] = ((pal & 0xf0) >> 4) | ((pal & 0x0f) << 4);
}

static void text_save_state(struct flanterm_context *_ctx) {
    struct textmode_context *ctx = (void *)_ctx;
    ctx->saved_state_text_palette = ctx->text_palette;
    ctx->saved_state_cursor_offset = ctx->cursor_offset;
}

static void text_restore_state(struct flanterm_context *_ctx) {
    struct textmode_context *ctx = (void *)_ctx;
    ctx->text_palette = ctx->saved_state_text_palette;
    ctx->cursor_offset = ctx->saved_state_cursor_offset;
}

static void text_swap_palette(struct flanterm_context *_ctx) {
    struct textmode_context *ctx = (void *)_ctx;
    ctx->text_palette = (ctx->text_palette << 4) | (ctx->text_palette >> 4);
}

static void text_scroll(struct flanterm_context *_ctx) {
    struct textmode_context *ctx = (void *)_ctx;

    // move the text up by one row
    for (size_t i = _ctx->scroll_top_margin * VD_COLS;
         i < (_ctx->scroll_bottom_margin - 1) * VD_COLS; i++) {
        ctx->back_buffer[i] = ctx->back_buffer[i + VD_COLS];
    }
    // clear the last line of the screen
    for (size_t i = (_ctx->scroll_bottom_margin - 1) * VD_COLS;
         i < _ctx->scroll_bottom_margin * VD_COLS; i += 2) {
        ctx->back_buffer[i] = ' ';
        ctx->back_buffer[i + 1] = ctx->text_palette;
    }
}

static void text_revscroll(struct flanterm_context *_ctx) {
    struct textmode_context *ctx = (void *)_ctx;

    // move the text up by one row
    for (size_t i = (_ctx->scroll_bottom_margin - 1) * VD_COLS - 2; ; i--) {
        ctx->back_buffer[i + VD_COLS] = ctx->back_buffer[i];
        if (i == _ctx->scroll_top_margin * VD_COLS) {
            break;
        }
    }
    // clear the first line of the screen
    for (size_t i = _ctx->scroll_top_margin * VD_COLS;
         i < (_ctx->scroll_top_margin + 1) * VD_COLS; i += 2) {
        ctx->back_buffer[i] = ' ';
        ctx->back_buffer[i + 1] = ctx->text_palette;
    }
}

static void text_clear(struct flanterm_context *_ctx, bool move) {
    struct textmode_context *ctx = (void *)_ctx;

    for (size_t i = 0; i < VIDEO_BOTTOM; i += 2) {
        ctx->back_buffer[i] = ' ';
        ctx->back_buffer[i + 1] = ctx->text_palette;
    }
    if (move) {
        ctx->cursor_offset = 0;
    }
}

static void text_full_refresh(struct flanterm_context *_ctx) {
    struct textmode_context *ctx = (void *)_ctx;

    for (size_t i = 0; i < VD_ROWS * VD_COLS; i++) {
        ctx->video_mem[i] = ctx->front_buffer[i];
        ctx->back_buffer[i] = ctx->front_buffer[i];
    }

    if (_ctx->cursor_enabled) {
        draw_cursor(ctx);
        ctx->old_cursor_offset = ctx->cursor_offset;
    }
}

static void text_double_buffer_flush(struct flanterm_context *_ctx) {
    struct textmode_context *ctx = (void *)_ctx;

    if (_ctx->cursor_enabled) {
        draw_cursor(ctx);
    }

    if (ctx->cursor_offset != ctx->old_cursor_offset || _ctx->cursor_enabled == false) {
        ctx->video_mem[ctx->old_cursor_offset + 1] = ctx->back_buffer[ctx->old_cursor_offset + 1];
    }

    for (size_t i = 0; i < VD_ROWS * VD_COLS; i++) {
        if (ctx->back_buffer[i] == ctx->front_buffer[i]) {
            continue;
        }

        ctx->front_buffer[i] = ctx->back_buffer[i];

        if (_ctx->cursor_enabled && i == ctx->cursor_offset + 1) {
            continue;
        }

        ctx->video_mem[i] = ctx->back_buffer[i];
    }

    if (_ctx->cursor_enabled) {
        ctx->old_cursor_offset = ctx->cursor_offset;
    }
}

static void text_get_cursor_pos(struct flanterm_context *_ctx, size_t *x, size_t *y) {
    struct textmode_context *ctx = (void *)_ctx;

    *x = (ctx->cursor_offset % VD_COLS) / 2;
    *y = ctx->cursor_offset / VD_COLS;
}

static void text_move_character(struct flanterm_context *_ctx, size_t new_x, size_t new_y, size_t old_x, size_t old_y) {
    struct textmode_context *ctx = (void *)_ctx;

    if (old_x >= VD_COLS / 2 || old_y >= VD_ROWS
     || new_x >= VD_COLS / 2 || new_y >= VD_ROWS) {
        return;
    }

    ctx->back_buffer[new_y * VD_COLS + new_x * 2] = ctx->back_buffer[old_y * VD_COLS + old_x * 2];
    ctx->back_buffer[new_y * VD_COLS + new_x * 2 + 1] = ctx->back_buffer[old_y * VD_COLS + old_x * 2 + 1];
}

static void text_set_cursor_pos(struct flanterm_context *_ctx, size_t x, size_t y) {
    struct textmode_context *ctx = (void *)_ctx;

    if (x >= VD_COLS / 2) {
        if ((int)x < 0) {
            x = 0;
        } else {
            x = VD_COLS / 2 - 1;
        }
    }
    if (y >= VD_ROWS) {
        if ((int)y < 0) {
            y = 0;
        } else {
            y = VD_ROWS - 1;
        }
    }
    ctx->cursor_offset = y * VD_COLS + x * 2;
    ctx->cursor_overflow = false;
}

static uint8_t ansi_colours[] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static void text_set_text_fg(struct flanterm_context *_ctx, size_t fg) {
    struct textmode_context *ctx = (void *)_ctx;
    ctx->text_palette = (ctx->text_palette & 0xf0) | ansi_colours[fg];
}

static void text_set_text_bg(struct flanterm_context *_ctx, size_t bg) {
    struct textmode_context *ctx = (void *)_ctx;
    ctx->text_palette = (ctx->text_palette & 0x0f) | (ansi_colours[bg] << 4);
}

static void text_set_text_fg_bright(struct flanterm_context *_ctx, size_t fg) {
    struct textmode_context *ctx = (void *)_ctx;
    ctx->text_palette = (ctx->text_palette & 0xf0) | (ansi_colours[fg] | (1 << 3));
}

static void text_set_text_bg_bright(struct flanterm_context *_ctx, size_t bg) {
    struct textmode_context *ctx = (void *)_ctx;
    ctx->text_palette = (ctx->text_palette & 0x0f) | ((ansi_colours[bg] | (1 << 3)) << 4);
}

static void text_set_text_fg_rgb(struct flanterm_context *ctx, uint32_t n) {
    (void)ctx;
    (void)n;
}

static void text_set_text_bg_rgb(struct flanterm_context *ctx, uint32_t n) {
    (void)ctx;
    (void)n;
}

static void text_set_text_fg_default(struct flanterm_context *_ctx) {
    struct textmode_context *ctx = (void *)_ctx;
    ctx->text_palette = (ctx->text_palette & 0xf0) | 7;
}

static void text_set_text_bg_default(struct flanterm_context *_ctx) {
    struct textmode_context *ctx = (void *)_ctx;
    ctx->text_palette &= 0x0f;
}

static void text_set_text_fg_default_bright(struct flanterm_context *_ctx) {
    struct textmode_context *ctx = (void *)_ctx;
    ctx->text_palette = (ctx->text_palette & 0xf0) | (7 | (1 << 3));
}

static void text_set_text_bg_default_bright(struct flanterm_context *_ctx) {
    struct textmode_context *ctx = (void *)_ctx;
    ctx->text_palette = (ctx->text_palette & 0x0f) | ((1 << 3) << 4);
}

static void text_putchar(struct flanterm_context *_ctx, uint8_t c) {
    struct textmode_context *ctx = (void *)_ctx;

    // Handle overflow from previous putchar
    if (ctx->cursor_overflow) {
        ctx->cursor_overflow = false;
        if (_ctx->wrap_enabled
         && (ctx->cursor_offset / VD_COLS < _ctx->scroll_bottom_margin - 1
             || _ctx->scroll_enabled)) {
            ctx->cursor_offset -= ctx->cursor_offset % VD_COLS;
            ctx->cursor_offset += VD_COLS;
            if (ctx->cursor_offset / VD_COLS == _ctx->scroll_bottom_margin) {
                ctx->cursor_offset -= VD_COLS;
                text_scroll(_ctx);
            }
            if (ctx->cursor_offset >= VD_ROWS * VD_COLS) {
                ctx->cursor_offset = (VD_ROWS - 1) * VD_COLS;
            }
        } else {
            ctx->cursor_offset = ctx->cursor_offset - (ctx->cursor_offset % VD_COLS) + VD_COLS - 2;
        }
    }

    ctx->back_buffer[ctx->cursor_offset] = c;
    ctx->back_buffer[ctx->cursor_offset + 1] = ctx->text_palette;
    if (ctx->cursor_offset % VD_COLS == VD_COLS - 2) {
        // At last column - flag overflow for next putchar
        ctx->cursor_overflow = true;
    } else if (ctx->cursor_offset < (VIDEO_BOTTOM - 1)) {
        ctx->cursor_offset += 2;
    }
}

static void text_deinit(struct flanterm_context *_ctx, void (*_free)(void *, size_t)) {
    struct textmode_context *ctx = (void *)_ctx;

    if (ctx->back_buffer != NULL) {
        _free(ctx->back_buffer, VD_ROWS * VD_COLS);
        ctx->back_buffer = NULL;
    }

    if (ctx->front_buffer != NULL) {
        _free(ctx->front_buffer, VD_ROWS * VD_COLS);
        ctx->front_buffer = NULL;
    }

    pmm_free(ctx, sizeof(struct textmode_context));
}

void vga_textmode_init(bool managed) {
    term_notready();

    if (quiet) {
        return;
    }

    if (current_video_mode != 0x3) {
        struct rm_regs r = {0};
        r.eax = 0x0003;
        rm_int(0x10, &r, &r);

        current_video_mode = 0x3;
    }

    terms = ext_mem_alloc(sizeof(void *));
    terms_i = 1;

    terms[0] = ext_mem_alloc(sizeof(struct textmode_context));

    struct flanterm_context *term = terms[0];
    struct textmode_context *ctx = (void *)term;

    if (ctx->back_buffer == NULL) {
        ctx->back_buffer = ext_mem_alloc(VD_ROWS * VD_COLS);
    } else {
        memset(ctx->back_buffer, 0, VD_ROWS * VD_COLS);
    }
    if (ctx->front_buffer == NULL) {
        ctx->front_buffer = ext_mem_alloc(VD_ROWS * VD_COLS);
    } else {
        memset(ctx->front_buffer, 0, VD_ROWS * VD_COLS);
    }

    ctx->cursor_offset = 0;
    ctx->cursor_overflow = false;
    ctx->text_palette = 0x07;

    ctx->video_mem = (volatile uint8_t *)0xb8000;

    text_clear(term, false);

    // VGA cursor code taken from: https://wiki.osdev.org/Text_Mode_Cursor

    if (!managed) {
        term->cursor_enabled = false;

        outb(0x3d4, 0x0a);
        outb(0x3d5, (inb(0x3d5) & 0xc0) | 14);
        outb(0x3d4, 0x0b);
        outb(0x3d5, (inb(0x3d5) & 0xe0) | 15);
        outb(0x3d4, 0x0f);
        outb(0x3d5, 0);
        outb(0x3d4, 0x0e);
        outb(0x3d5, 0);

        struct rm_regs r = {0};
        r.eax = 0x0200;
        rm_int(0x10, &r, &r);
    } else {
        outb(0x3d4, 0x0a);
        outb(0x3d5, 0x20);
    }

    text_double_buffer_flush(term);

    if (managed && serial) {
        term->cols = 80;
        term->rows = 24;
    } else {
        term->cols = 80;
        term->rows = 25;
    }

    term->raw_putchar = text_putchar;
    term->clear = text_clear;
    term->set_cursor_pos = text_set_cursor_pos;
    term->get_cursor_pos = text_get_cursor_pos;
    term->set_text_fg = text_set_text_fg;
    term->set_text_bg = text_set_text_bg;
    term->set_text_fg_bright = text_set_text_fg_bright;
    term->set_text_bg_bright = text_set_text_bg_bright;
    term->set_text_fg_rgb = text_set_text_fg_rgb;
    term->set_text_bg_rgb = text_set_text_bg_rgb;
    term->set_text_fg_default = text_set_text_fg_default;
    term->set_text_bg_default = text_set_text_bg_default;
    term->set_text_fg_default_bright = text_set_text_fg_default_bright;
    term->set_text_bg_default_bright = text_set_text_bg_default_bright;
    term->move_character = text_move_character;
    term->scroll = text_scroll;
    term->revscroll = text_revscroll;
    term->swap_palette = text_swap_palette;
    term->save_state = text_save_state;
    term->restore_state = text_restore_state;
    term->double_buffer_flush = text_double_buffer_flush;
    term->full_refresh = text_full_refresh;
    term->deinit = text_deinit;

    flanterm_context_reinit(term);

    if (!managed) {
        term->cursor_enabled = false;
    }

    term->full_refresh(term);

    if (!managed) {
        term->deinit(term, pmm_free_size_t);
        pmm_free(terms, sizeof(void *));
        terms_i = 0;
        terms = NULL;
        term_backend = _NOT_READY;
    } else {
        term_backend = TEXTMODE;
    }
}

#endif
