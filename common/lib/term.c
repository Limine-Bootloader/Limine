#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/term.h>
#include <lib/real.h>
#include <lib/misc.h>
#include <lib/fb.h>
#include <mm/pmm.h>
#include <drivers/vga_textmode.h>
#include <flanterm_backends/fb.h>

#if defined (BIOS)
int current_video_mode = -1;
#endif

struct flanterm_context **terms = NULL;
size_t terms_i = 0;

int term_backend = _NOT_READY;

void term_notready(void) {
    for (size_t i = 0; i < terms_i; i++) {
        struct flanterm_context *term = terms[i];

        term->deinit(term, pmm_free);
    }

    pmm_free(terms, terms_i * sizeof(void *));

    terms_i = 0;
    terms = NULL;

    term_backend = _NOT_READY;
}

// --- fallback ---

#if defined (BIOS)
static void fallback_raw_putchar(struct flanterm_context *ctx, uint8_t c) {
    (void)ctx;
    struct rm_regs r = {0};
    r.eax = 0x0e00 | c;
    rm_int(0x10, &r, &r);
}

static void fallback_set_cursor_pos(struct flanterm_context *ctx, size_t x, size_t y);
static void fallback_get_cursor_pos(struct flanterm_context *ctx, size_t *x, size_t *y);

static void fallback_clear(struct flanterm_context *ctx, bool move) {
    (void)ctx;
    size_t x, y;
    fallback_get_cursor_pos(NULL, &x, &y);
    struct rm_regs r = {0};
    rm_int(0x11, &r, &r);
    switch ((r.eax >> 4) & 3) {
        case 0:
            r.eax = 3;
            break;
        case 1:
            r.eax = 1;
            break;
        case 2:
            r.eax = 3;
            break;
        case 3:
            r.eax = 7;
            break;
    }
    rm_int(0x10, &r, &r);
    if (move) {
        x = y = 0;
    }
    fallback_set_cursor_pos(NULL, x, y);
}

static void fallback_set_cursor_pos(struct flanterm_context *ctx, size_t x, size_t y) {
    (void)ctx;
    struct rm_regs r = {0};
    r.eax = 0x0200;
    r.ebx = 0;
    r.edx = (y << 8) + x;
    rm_int(0x10, &r, &r);
}

static void fallback_get_cursor_pos(struct flanterm_context *ctx, size_t *x, size_t *y) {
    (void)ctx;
    struct rm_regs r = {0};
    r.eax = 0x0300;
    r.ebx = 0;
    rm_int(0x10, &r, &r);
    *x = r.edx & 0xff;
    *y = r.edx >> 8;
}

static void fallback_scroll(struct flanterm_context *ctx) {
    (void)ctx;
    size_t x, y;
    fallback_get_cursor_pos(NULL, &x, &y);
    fallback_set_cursor_pos(NULL, ctx->cols - 1, ctx->rows - 1);
    fallback_raw_putchar(NULL, ' ');
    fallback_set_cursor_pos(NULL, x, y);
}

#elif defined (UEFI)

static size_t cursor_x = 0, cursor_y = 0;

static void fallback_scroll(struct flanterm_context *ctx) {
    (void)ctx;
    UINTN uefi_x_size, uefi_y_size;
    gST->ConOut->QueryMode(gST->ConOut, gST->ConOut->Mode->Mode, &uefi_x_size, &uefi_y_size);
    gST->ConOut->SetCursorPosition(gST->ConOut, uefi_x_size - 1, uefi_y_size - 1);
    CHAR16 string[2];
    string[0] = ' ';
    string[1] = 0;
    gST->ConOut->OutputString(gST->ConOut, string);
    gST->ConOut->SetCursorPosition(gST->ConOut, cursor_x, cursor_y);
}

static void fallback_raw_putchar(struct flanterm_context *ctx, uint8_t c) {
    if (!ctx->scroll_enabled && cursor_x == ctx->cols - 1 && cursor_y == ctx->rows - 1) {
        return;
    }
    gST->ConOut->EnableCursor(gST->ConOut, true);
    CHAR16 string[2];
    string[0] = c;
    string[1] = 0;
    gST->ConOut->OutputString(gST->ConOut, string);
    if (++cursor_x >= ctx->cols) {
        cursor_x = 0;
        if (++cursor_y >= ctx->rows) {
            cursor_y--;
        }
    }
    gST->ConOut->SetCursorPosition(gST->ConOut, cursor_x, cursor_y);
}

static void fallback_clear(struct flanterm_context *ctx, bool move) {
    (void)ctx;
    gST->ConOut->ClearScreen(gST->ConOut);
    if (move) {
        cursor_x = cursor_y = 0;
    }
    gST->ConOut->SetCursorPosition(gST->ConOut, cursor_x, cursor_y);
}

static void fallback_set_cursor_pos(struct flanterm_context *ctx, size_t x, size_t y) {
    (void)ctx;
    if (x >= ctx->cols || y >= ctx->rows) {
        return;
    }
    gST->ConOut->SetCursorPosition(gST->ConOut, x, y);
    cursor_x = x;
    cursor_y = y;
}

static void fallback_get_cursor_pos(struct flanterm_context *ctx, size_t *x, size_t *y) {
    (void)ctx;
    *x = cursor_x;
    *y = cursor_y;
}

static UINTN ansi_colours[] = {
    EFI_BLACK,
    EFI_RED,
    EFI_GREEN,
    EFI_YELLOW,
    EFI_BLUE,
    EFI_MAGENTA,
    EFI_CYAN,
    EFI_LIGHTGRAY
};

static UINTN conout_current_fg, conout_current_bg;

static void fallback_set_text_fg(struct flanterm_context *ctx, size_t fg) {
    (void)ctx;
    conout_current_fg = ansi_colours[fg];
    gST->ConOut->SetAttribute(gST->ConOut, EFI_TEXT_ATTR(conout_current_fg, conout_current_bg));
}

static void fallback_set_text_bg(struct flanterm_context *ctx, size_t bg) {
    (void)ctx;
    conout_current_bg = ansi_colours[bg];
    gST->ConOut->SetAttribute(gST->ConOut, EFI_TEXT_ATTR(conout_current_fg, conout_current_bg));
}

static void fallback_set_text_fg_bright(struct flanterm_context *ctx, size_t fg) {
    (void)ctx;
    conout_current_fg = ansi_colours[fg] | EFI_BRIGHT;
    gST->ConOut->SetAttribute(gST->ConOut, EFI_TEXT_ATTR(conout_current_fg, conout_current_bg));
}

static void fallback_set_text_bg_bright(struct flanterm_context *ctx, size_t bg) {
    (void)ctx;
    // bg does not support bright
    conout_current_bg = ansi_colours[bg];
    gST->ConOut->SetAttribute(gST->ConOut, EFI_TEXT_ATTR(conout_current_fg, conout_current_bg));
}

static void fallback_set_text_fg_default(struct flanterm_context *ctx) {
    (void)ctx;
    conout_current_fg = EFI_LIGHTGRAY;
    gST->ConOut->SetAttribute(gST->ConOut, EFI_TEXT_ATTR(conout_current_fg, conout_current_bg));
}

static void fallback_set_text_bg_default(struct flanterm_context *ctx) {
    (void)ctx;
    conout_current_bg = EFI_BLACK;
    gST->ConOut->SetAttribute(gST->ConOut, EFI_TEXT_ATTR(conout_current_fg, conout_current_bg));
}

static void fallback_swap_palette(struct flanterm_context *ctx) {
    (void)ctx;
    UINTN tmp = conout_current_bg;
    conout_current_bg = conout_current_fg;
    conout_current_fg = tmp;
    gST->ConOut->SetAttribute(gST->ConOut, EFI_TEXT_ATTR(conout_current_fg, conout_current_bg));
}

#endif

static bool dummy_handle(void) {
    return true;
}

void term_fallback(void) {
    term_notready();

    terms = ext_mem_alloc(sizeof(void *));
    terms_i = 1;

    terms[0] = ext_mem_alloc(sizeof(struct flanterm_context));

    struct flanterm_context *term = terms[0];

#if defined (UEFI)
    if (!efi_boot_services_exited) {
#endif

        fallback_clear(NULL, true);

        term->set_text_fg = (void *)dummy_handle;
        term->set_text_bg = (void *)dummy_handle;
        term->set_text_fg_bright = (void *)dummy_handle;
        term->set_text_bg_bright = (void *)dummy_handle;
        term->set_text_fg_rgb = (void *)dummy_handle;
        term->set_text_bg_rgb = (void *)dummy_handle;
        term->set_text_fg_default = (void *)dummy_handle;
        term->set_text_bg_default = (void *)dummy_handle;
        term->move_character = (void *)dummy_handle;
        term->revscroll = (void *)dummy_handle;
        term->swap_palette = (void *)dummy_handle;
        term->save_state = (void *)dummy_handle;
        term->restore_state = (void *)dummy_handle;
        term->double_buffer_flush = (void *)dummy_handle;
        term->full_refresh = (void *)dummy_handle;
        term->deinit = (void *)dummy_handle;

        term->raw_putchar = fallback_raw_putchar;
        term->clear = fallback_clear;
        term->set_cursor_pos = fallback_set_cursor_pos;
        term->get_cursor_pos = fallback_get_cursor_pos;
        term->scroll = fallback_scroll;
        term->cols = 80;
        term->rows = 24;
        term_backend = FALLBACK;
        flanterm_context_reinit(term);
#if defined (UEFI)

        term->set_text_fg = fallback_set_text_fg;
        term->set_text_bg = fallback_set_text_bg;
        term->set_text_fg_bright = fallback_set_text_fg_bright;
        term->set_text_bg_bright = fallback_set_text_bg_bright;
        term->set_text_fg_default = fallback_set_text_fg_default;
        term->set_text_bg_default = fallback_set_text_bg_default;
        term->swap_palette = fallback_swap_palette;

        term->set_text_fg_default(term);
        term->set_text_bg_default(term);
    } else {
        if (fb_fbs_count == 0) {
            goto fail;
        }

        terms[0] = flanterm_fb_init(ext_mem_alloc, pmm_free,
            (void *)(uintptr_t)fb_fbs[0].framebuffer_addr, fb_fbs[0].framebuffer_width,
            fb_fbs[0].framebuffer_height, fb_fbs[0].framebuffer_pitch,
            fb_fbs[0].red_mask_size, fb_fbs[0].red_mask_shift,
            fb_fbs[0].green_mask_size, fb_fbs[0].green_mask_shift,
            fb_fbs[0].blue_mask_size, fb_fbs[0].blue_mask_shift,
            NULL,
            NULL, NULL,
            NULL, NULL,
            NULL, NULL,
            NULL, 0, 0, 1,
            0, 0,
            0
        );
    }

    return;

fail:
    pmm_free(terms[0], sizeof(struct flanterm_context));
    pmm_free(terms, sizeof(void *));
    terms_i = 0;
#endif
}
