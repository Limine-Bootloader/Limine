#ifndef DRIVERS__VGA_TEXTMODE_H__
#define DRIVERS__VGA_TEXTMODE_H__

#if defined (BIOS)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <flanterm.h>

#define VD_COLS (80 * 2)
#define VD_ROWS 25

struct textmode_context {
    struct flanterm_context term;

    volatile uint8_t *video_mem;

    uint8_t *back_buffer;
    uint8_t *front_buffer;

    size_t cursor_offset;
    size_t old_cursor_offset;
    bool cursor_status;
    uint8_t text_palette;

    uint8_t saved_state_text_palette;
    size_t saved_state_cursor_offset;
};

void vga_textmode_init(bool managed);

#endif

#endif
