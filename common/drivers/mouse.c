#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <drivers/mouse.h>
#include <lib/config.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/term.h>
#include <mm/pmm.h>
#include <flanterm.h>
#include <flanterm_backends/fb.h>
#if defined (BIOS)
#  include <sys/cpu.h>
#elif defined (UEFI)
#  include <efi.h>
#endif

static bool mouse_active = false;

// Pointer position is in the resolution of 1/256th of a terminal cell. The
// position and visibility survive the menu data rewind so that the pointer does not
// jump.
static no_unwind int64_t pos_x, pos_y;
static no_unwind bool pointer_pos_valid = false;

// The pointer stays hidden until the mouse is first used; a plugged
// in but unused mouse does not clutter the menu.
static no_unwind bool pointer_shown = false;

static bool left_down, right_down;

// A click is only reported for a press+release pair where the press edge was
// observed after the last flush; this stops clicks buffered before a failed
// boot from being replayed once the menu is re-entered.
static bool seen_released;
static bool press_seen;
static size_t press_row;

static bool moved_pending;
static bool click_pending;
static size_t click_cell_x, click_cell_y;
static int wheel_pending;

static void reset_transient_state(void) {
    moved_pending = false;
    click_pending = false;
    wheel_pending = 0;
    press_seen = false;
    seen_released = !left_down;
}

static void clamp_pointer(void) {
    int64_t max_x = (int64_t)terms[0]->cols * 256 - 1;
    int64_t max_y = (int64_t)terms[0]->rows * 256 - 1;

    if (pos_x < 0) {
        pos_x = 0;
    } else if (pos_x > max_x) {
        pos_x = max_x;
    }
    if (pos_y < 0) {
        pos_y = 0;
    } else if (pos_y > max_y) {
        pos_y = max_y;
    }
}

static void place_pointer(void) {
    if (terms_i == 0) {
        return;
    }
    if (pointer_pos_valid) {
        clamp_pointer();
        return;
    }
    pointer_pos_valid = true;
    pos_x = (int64_t)(terms[0]->cols / 2) * 256;
    pos_y = (int64_t)(terms[0]->rows / 2) * 256;
}

static int move_by(int64_t dx, int64_t dy) {
    if (terms_i == 0) {
        return 0;
    }

    int64_t old_x = pos_x, old_y = pos_y;

    pos_x += dx;
    pos_y += dy;
    clamp_pointer();

    if (pos_x != old_x || pos_y != old_y) {
        moved_pending = true;
        pointer_shown = true;
        return MOUSE_EVENT_MOVE;
    }
    return 0;
}

static int update_buttons(bool left, bool right) {
    int ev = 0;

    if (left != left_down) {
        ev |= MOUSE_EVENT_BUTTON;
        pointer_shown = true;
        if (left) {
            if (seen_released) {
                press_seen = true;
                press_row = pos_y >> 8;
            }
        } else {
            if (press_seen && press_row == (size_t)(pos_y >> 8)) {
                click_pending = true;
                click_cell_x = pos_x >> 8;
                click_cell_y = pos_y >> 8;
            }
            press_seen = false;
        }
        left_down = left;
    }
    if (!left) {
        seen_released = true;
    }

    if (right != right_down) {
        ev |= MOUSE_EVENT_BUTTON;
        pointer_shown = true;
        right_down = right;
    }

    return ev;
}

static int wheel_by(int steps) {
    if (steps == 0) {
        return 0;
    }
    wheel_pending += steps;
    pointer_shown = true;
    return MOUSE_EVENT_WHEEL;
}

static bool mouse_config_disabled(void) {
    char *conf = config_get_value(NULL, 0, "MOUSE");
    return conf != NULL && strcmp(conf, "no") == 0;
}

bool mouse_present(void) {
    return mouse_active;
}

bool mouse_state_pending(void) {
    return mouse_active && (moved_pending || click_pending || wheel_pending != 0);
}

void mouse_get_state(struct mouse_state *state) {
    state->x = pos_x >> 8;
    state->y = pos_y >> 8;
    state->wheel = wheel_pending;
    state->moved = moved_pending;
    state->click = click_pending;
    state->click_x = click_cell_x;
    state->click_y = click_cell_y;

    wheel_pending = 0;
    moved_pending = false;
    click_pending = false;
}

// Exported from the Enlightenment E16 BlueSteel theme cursor.
#define POINTER_W 16
#define POINTER_H 16
#define POINTER_HOT_X 1
#define POINTER_HOT_Y 1

static const char *pointer_shape[POINTER_H] = {
    "###             ",
    "#..##           ",
    "#....##         ",
    " #.....##       ",
    " #.......##     ",
    "  #........##   ",
    "  #..........#  ",
    "   #.......##   ",
    "   #......#     ",
    "    #......#    ",
    "    #...#...#   ",
    "     #.# #...#  ",
    "     #.#  #.#   ",
    "      #    #    ",
    "                ",
    "                ",
};

struct pointer_backing {
    struct flanterm_fb_context *fb_ctx;
    uint32_t *pixels;
    size_t pixels_size;
    bool drawn;
    size_t x, y, w, h;
};

static struct pointer_backing *backings;
static size_t backings_count;

static void free_backings(void) {
    if (backings == NULL) {
        return;
    }
    for (size_t i = 0; i < backings_count; i++) {
        if (backings[i].pixels != NULL) {
            pmm_free(backings[i].pixels, backings[i].pixels_size);
        }
    }
    pmm_free(backings, backings_count * sizeof(struct pointer_backing));
    backings = NULL;
    backings_count = 0;
}

static void init_backings(void) {
    free_backings();

    backings_count = terms_i;
    backings = ext_mem_alloc_counted(backings_count, sizeof(struct pointer_backing));

    for (size_t i = 0; i < terms_i; i++) {
        if (term_backend != GTERM) {
            continue;
        }
        struct flanterm_fb_context *ctx = (struct flanterm_fb_context *)terms[i];
        if (ctx->rotation != FLANTERM_FB_ROTATE_0) {
            continue;
        }
        backings[i].fb_ctx = ctx;
        backings[i].pixels_size = POINTER_W * ctx->font_scale_x
                                * POINTER_H * ctx->font_scale_y
                                * sizeof(uint32_t);
        backings[i].pixels = ext_mem_alloc(backings[i].pixels_size);
    }
}

static void pointer_erase_sprite(struct pointer_backing *bk) {
    struct flanterm_fb_context *ctx = bk->fb_ctx;

    if (!bk->drawn) {
        return;
    }
    bk->drawn = false;

    for (size_t row = 0; row < bk->h; row++) {
        volatile uint32_t *line = ctx->framebuffer
            + (bk->y + row) * (ctx->pitch / sizeof(uint32_t)) + bk->x;
        for (size_t col = 0; col < bk->w; col++) {
            line[col] = bk->pixels[row * bk->w + col];
        }
        if (ctx->flush_callback != NULL) {
            ctx->flush_callback(line, bk->w * sizeof(uint32_t));
        }
    }
}

static void pointer_draw_sprite(struct pointer_backing *bk) {
    struct flanterm_fb_context *ctx = bk->fb_ctx;

    size_t sx = ctx->font_scale_x, sy = ctx->font_scale_y;

    // Anchor the arrow tip on the pointer position
    int64_t px = (int64_t)(ctx->offset_x + (size_t)pos_x * ctx->glyph_width / 256)
               - (int64_t)(POINTER_HOT_X * sx);
    int64_t py = (int64_t)(ctx->offset_y + (size_t)pos_y * ctx->glyph_height / 256)
               - (int64_t)(POINTER_HOT_Y * sy);
    size_t clip_x = px < 0 ? -px : 0;
    size_t clip_y = py < 0 ? -py : 0;
    size_t x = px < 0 ? 0 : (size_t)px;
    size_t y = py < 0 ? 0 : (size_t)py;

    if (x >= ctx->width || y >= ctx->height
     || clip_x >= POINTER_W * sx || clip_y >= POINTER_H * sy) {
        return;
    }

    uint32_t white = ((uint32_t)(0xff >> (8 - ctx->red_mask_size)) << ctx->red_mask_shift)
                   | ((uint32_t)(0xff >> (8 - ctx->green_mask_size)) << ctx->green_mask_shift)
                   | ((uint32_t)(0xff >> (8 - ctx->blue_mask_size)) << ctx->blue_mask_shift);
    uint32_t black = 0;

    size_t w = POINTER_W * sx - clip_x, h = POINTER_H * sy - clip_y;
    if (x + w > ctx->width) {
        w = ctx->width - x;
    }
    if (y + h > ctx->height) {
        h = ctx->height - y;
    }

    bk->x = x;
    bk->y = y;
    bk->w = w;
    bk->h = h;

    for (size_t row = 0; row < h; row++) {
        volatile uint32_t *line = ctx->framebuffer
            + (y + row) * (ctx->pitch / sizeof(uint32_t)) + x;
        const char *shape_row = pointer_shape[(row + clip_y) / sy];
        for (size_t col = 0; col < w; col++) {
            bk->pixels[row * w + col] = line[col];
            char c = shape_row[(col + clip_x) / sx];
            if (c == '#') {
                line[col] = black;
            } else if (c == '.') {
                line[col] = white;
            }
        }
        if (ctx->flush_callback != NULL) {
            ctx->flush_callback(line, w * sizeof(uint32_t));
        }
    }

    bk->drawn = true;
}

void mouse_erase_pointer(void) {
    if (!mouse_active) {
        return;
    }
    for (size_t i = 0; i < backings_count && i < terms_i; i++) {
        if (backings[i].pixels != NULL) {
            pointer_erase_sprite(&backings[i]);
        } else {
            terms[i]->cursor_enabled = false;
        }
    }
}

static void render_pointer(bool sprites_only) {
    if (!mouse_active || !pointer_shown) {
        return;
    }
    for (size_t i = 0; i < backings_count && i < terms_i; i++) {
        if (backings[i].pixels != NULL) {
            pointer_erase_sprite(&backings[i]);
            pointer_draw_sprite(&backings[i]);
        } else if (!sprites_only) {
            flanterm_set_cursor_pos(terms[i], pos_x >> 8, pos_y >> 8);
            terms[i]->cursor_enabled = true;
            terms[i]->double_buffer_flush(terms[i]);
        }
    }
}

void mouse_render_pointer(void) {
    render_pointer(false);
}

void mouse_render_pointer_overlay(void) {
    render_pointer(true);
}

#if defined (BIOS)

#define I8042_DATA 0x60
#define I8042_STATUS 0x64
#define I8042_COMMAND 0x64

#define I8042_STATUS_OUT_FULL 0x01
#define I8042_STATUS_IN_FULL 0x02
#define I8042_STATUS_AUX_DATA 0x20

// Default PS/2 resolution is 4 counts/mm; use 8 counts (2mm) per cell
// horizontally and 16 counts (4mm) vertically.
#define COUNTS_TO_FIXED_X(d) ((int64_t)(d) * 256 / 8)
#define COUNTS_TO_FIXED_Y(d) ((int64_t)(d) * 256 / 16)

static bool wheel_packets;
static uint8_t packet[4];
static size_t packet_index;
static uint8_t saved_command_byte;
static bool have_saved_command_byte;

static bool i8042_wait_write(void) {
    for (size_t i = 0; i < 10000; i++) {
        if ((inb(I8042_STATUS) & I8042_STATUS_IN_FULL) == 0) {
            return true;
        }
        stall(10);
    }
    return false;
}

static int i8042_read_data(bool *is_aux) {
    for (size_t i = 0; i < 10000; i++) {
        uint8_t status = inb(I8042_STATUS);
        if (status & I8042_STATUS_OUT_FULL) {
            *is_aux = (status & I8042_STATUS_AUX_DATA) != 0;
            return inb(I8042_DATA);
        }
        stall(10);
    }
    return -1;
}

static int aux_read(void) {
    // Discard interleaved keyboard bytes, only mouse init reads these.
    for (size_t i = 0; i < 16; i++) {
        bool is_aux;
        int b = i8042_read_data(&is_aux);
        if (b == -1) {
            return -1;
        }
        if (is_aux) {
            return b;
        }
    }
    return -1;
}

static bool aux_write(uint8_t val) {
    if (!i8042_wait_write()) {
        return false;
    }
    outb(I8042_COMMAND, 0xd4);
    if (!i8042_wait_write()) {
        return false;
    }
    outb(I8042_DATA, val);
    return true;
}

static bool aux_command(uint8_t val) {
    if (!aux_write(val)) {
        return false;
    }
    // Tolerate stale motion bytes queued ahead of the acknowledge.
    for (size_t i = 0; i < 64; i++) {
        int b = aux_read();
        if (b == -1 || b == 0xfe || b == 0xfc) {
            return false;
        }
        if (b == 0xfa) {
            return true;
        }
    }
    return false;
}

static void aux_drain(void) {
    for (size_t i = 0; i < 128; i++) {
        if ((inb(I8042_STATUS) & I8042_STATUS_OUT_FULL) == 0) {
            break;
        }
        inb(I8042_DATA);
        stall(100);
    }
    packet_index = 0;
}

static bool restore_command_byte(void) {
    if (!have_saved_command_byte) {
        return true;
    }
    have_saved_command_byte = false;
    if (!i8042_wait_write()) {
        return false;
    }
    outb(I8042_COMMAND, 0x60);
    if (!i8042_wait_write()) {
        return false;
    }
    outb(I8042_DATA, saved_command_byte);
    return true;
}

bool mouse_init(void) {
    bool is_aux;
    int b;

    mouse_deinit();

    if (mouse_config_disabled() || terms_i == 0) {
        return false;
    }

    if (inb(I8042_STATUS) == 0xff) {
        return false;
    }

    aux_drain();

    // Aux interface test
    if (!i8042_wait_write()) {
        return false;
    }
    outb(I8042_COMMAND, 0xa9);
    if (i8042_read_data(&is_aux) != 0x00) {
        return false;
    }

    if (!i8042_wait_write()) {
        return false;
    }
    outb(I8042_COMMAND, 0x20);
    b = i8042_read_data(&is_aux);
    if (b == -1) {
        return false;
    }
    saved_command_byte = b;
    have_saved_command_byte = true;

    // Aux IRQ off (bit 1) so the BIOS cannot steal packets
    if (!i8042_wait_write()) {
        goto fail;
    }
    outb(I8042_COMMAND, 0x60);
    if (!i8042_wait_write()) {
        goto fail;
    }
    outb(I8042_DATA, (b & ~0x22));

    if (!i8042_wait_write()) {
        goto fail;
    }
    outb(I8042_COMMAND, 0xa8);

    if (!aux_command(0xf5)) {
        goto fail;
    }
    aux_drain();
    if (!aux_command(0xf6)) {
        goto fail;
    }

    // IntelliMouse magic sample rate sequence to unlock the wheel.
    // from here: https://wiki.osdev.org/PS/2_Mouse
    wheel_packets = false;
    if (aux_command(0xf3) && aux_command(200)
     && aux_command(0xf3) && aux_command(100)
     && aux_command(0xf3) && aux_command(80)
     && aux_command(0xf2)) {
        wheel_packets = aux_read() == 3;
    }

    // Seed current button state so a button held across a failed boot does
    // not turn into a click on release
    left_down = false;
    right_down = false;
    if (aux_command(0xe9)) {
        b = aux_read();
        if (b != -1) {
            left_down = (b & 0x01) != 0;
            right_down = (b & 0x02) != 0;
        }
        aux_read();
        aux_read();
    }

    if (!aux_command(0xf4)) {
        goto fail;
    }

    aux_drain();
    reset_transient_state();
    place_pointer();
    moved_pending = pointer_shown;
    init_backings();
    mouse_active = true;
    return true;

fail:
    restore_command_byte();
    return false;
}

void mouse_deinit(void) {
    if (mouse_active) {
        mouse_erase_pointer();
        mouse_active = false;
        aux_command(0xf5);
        aux_drain();
    }
    restore_command_byte();
    free_backings();
}

static int process_packet_byte(uint8_t b) {
    // First byte of a packet must have bit 3 set; drop bytes until it does
    // so that a missed byte resynchronises the stream.
    if (packet_index == 0 && (b & 0x08) == 0) {
        return 0;
    }

    packet[packet_index++] = b;
    if (packet_index < (wheel_packets ? 4u : 3u)) {
        return 0;
    }
    packet_index = 0;

    int ev = 0;

    if ((packet[0] & 0xc0) == 0) {
        int dx = packet[1] - ((packet[0] << 4) & 0x100);
        int dy = packet[2] - ((packet[0] << 3) & 0x100);
        // PS/2 positive Y means "up", screen Y grows downwards.
        ev |= move_by(COUNTS_TO_FIXED_X(dx), COUNTS_TO_FIXED_Y(-dy));
    }

    ev |= update_buttons((packet[0] & 0x01) != 0, (packet[0] & 0x02) != 0);

    if (wheel_packets) {
        ev |= wheel_by((int8_t)packet[3]);
    }

    return ev;
}

int mouse_process_pending(void) {
    int ev = 0;

    if (!mouse_active) {
        return 0;
    }

    for (size_t i = 0; i < 256; i++) {
        uint8_t status = inb(I8042_STATUS);
        if ((status & (I8042_STATUS_OUT_FULL | I8042_STATUS_AUX_DATA))
         != (I8042_STATUS_OUT_FULL | I8042_STATUS_AUX_DATA)) {
            break;
        }
        ev |= process_packet_byte(inb(I8042_DATA));
    }

    return ev;
}

void mouse_flush(void) {
    if (!mouse_active) {
        return;
    }
    aux_drain();
    reset_transient_state();
}

#elif defined (UEFI)

#define MAX_POINTERS 8

// 2mm per cell horizontally, 4mm per cell vertically.
#define MM_PER_CELL_X 2
#define MM_PER_CELL_Y 4

static EFI_SIMPLE_POINTER_PROTOCOL *rel_pointers[MAX_POINTERS];
static size_t rel_pointer_count;
static EFI_ABSOLUTE_POINTER_PROTOCOL *abs_pointers[MAX_POINTERS];
static size_t abs_pointer_count;
static uint32_t discard_first_move;

static size_t locate_pointers(EFI_GUID *guid, void **out, size_t max) {
    void *iface;

    // Prefer the console splitter aggregate
    if (gST->ConsoleInHandle != NULL
     && gBS->HandleProtocol(gST->ConsoleInHandle, guid, &iface) == EFI_SUCCESS) {
        out[0] = iface;
        return 1;
    }

    UINTN count = 0;
    EFI_HANDLE *handles = NULL;
    if (gBS->LocateHandleBuffer(ByProtocol, guid, NULL, &count, &handles) != EFI_SUCCESS) {
        return 0;
    }

    size_t n = 0;
    for (UINTN i = 0; i < count && n < max; i++) {
        if (gBS->HandleProtocol(handles[i], guid, &iface) == EFI_SUCCESS) {
            out[n++] = iface;
        }
    }
    gBS->FreePool(handles);
    return n;
}

bool mouse_init(void) {
    EFI_GUID rel_guid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_GUID abs_guid = EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;

    mouse_deinit();

    if (mouse_config_disabled() || terms_i == 0 || efi_boot_services_exited) {
        return false;
    }

    rel_pointer_count = locate_pointers(&rel_guid, (void **)rel_pointers, MAX_POINTERS);
    abs_pointer_count = locate_pointers(&abs_guid, (void **)abs_pointers, MAX_POINTERS);

    if (rel_pointer_count + abs_pointer_count == 0) {
        return false;
    }

    for (size_t i = 0; i < rel_pointer_count; i++) {
        rel_pointers[i]->Reset(rel_pointers[i], false);
    }
    for (size_t i = 0; i < abs_pointer_count; i++) {
        abs_pointers[i]->Reset(abs_pointers[i], false);
    }

    discard_first_move = (1 << rel_pointer_count) - 1;

    left_down = false;
    right_down = false;
    reset_transient_state();
    seen_released = false;
    place_pointer();
    moved_pending = pointer_shown;
    init_backings();
    mouse_active = true;
    return true;
}

void mouse_deinit(void) {
    if (mouse_active) {
        mouse_erase_pointer();
    }
    mouse_active = false;
    rel_pointer_count = 0;
    abs_pointer_count = 0;
    free_backings();
}

size_t mouse_get_efi_events(EFI_EVENT *events, size_t max) {
    size_t n = 0;

    if (!mouse_active) {
        return 0;
    }

    for (size_t i = 0; i < rel_pointer_count && n < max; i++) {
        events[n++] = rel_pointers[i]->WaitForInput;
    }
    for (size_t i = 0; i < abs_pointer_count && n < max; i++) {
        events[n++] = abs_pointers[i]->WaitForInput;
    }
    return n;
}

static int set_absolute_position(EFI_ABSOLUTE_POINTER_MODE *mode,
                                 EFI_ABSOLUTE_POINTER_STATE *state) {
    if (terms_i == 0
     || mode->AbsoluteMaxX <= mode->AbsoluteMinX
     || mode->AbsoluteMaxY <= mode->AbsoluteMinY) {
        return 0;
    }

    int64_t new_x = (int64_t)((state->CurrentX - mode->AbsoluteMinX)
                  * ((uint64_t)terms[0]->cols * 256)
                  / (mode->AbsoluteMaxX - mode->AbsoluteMinX));
    int64_t new_y = (int64_t)((state->CurrentY - mode->AbsoluteMinY)
                  * ((uint64_t)terms[0]->rows * 256)
                  / (mode->AbsoluteMaxY - mode->AbsoluteMinY));

    return move_by(new_x - pos_x, new_y - pos_y);
}

int mouse_handle_efi_event(size_t index) {
    if (!mouse_active) {
        return 0;
    }

    if (index < rel_pointer_count) {
        EFI_SIMPLE_POINTER_PROTOCOL *p = rel_pointers[index];
        EFI_SIMPLE_POINTER_STATE state;
        if (p->GetState(p, &state) != EFI_SUCCESS) {
            return 0;
        }

        int ev = 0;

        if (discard_first_move & (1 << index)) {
            discard_first_move &= ~(1 << index);
        } else {
            int64_t res_x = p->Mode->ResolutionX != 0 ? p->Mode->ResolutionX : 4;
            int64_t res_y = p->Mode->ResolutionY != 0 ? p->Mode->ResolutionY : 4;
            ev |= move_by((int64_t)state.RelativeMovementX * 256 / (res_x * MM_PER_CELL_X),
                          (int64_t)state.RelativeMovementY * 256 / (res_y * MM_PER_CELL_Y));

            if (p->Mode->ResolutionZ != 0 && state.RelativeMovementZ != 0) {
                int steps = state.RelativeMovementZ / (int64_t)p->Mode->ResolutionZ;
                if (steps == 0) {
                    steps = state.RelativeMovementZ > 0 ? 1 : -1;
                }
                ev |= wheel_by(steps);
            }
        }

        ev |= update_buttons(state.LeftButton, state.RightButton);
        return ev;
    }
    index -= rel_pointer_count;

    if (index < abs_pointer_count) {
        EFI_ABSOLUTE_POINTER_PROTOCOL *p = abs_pointers[index];
        EFI_ABSOLUTE_POINTER_STATE state;
        if (p->GetState(p, &state) != EFI_SUCCESS) {
            return 0;
        }

        int ev = set_absolute_position(p->Mode, &state);
        ev |= update_buttons((state.ActiveButtons & EFI_ABSP_TouchActive) != 0,
                             (state.ActiveButtons & EFI_ABS_AltActive) != 0);
        return ev;
    }

    return 0;
}

void mouse_flush(void) {
    if (!mouse_active) {
        return;
    }
    for (size_t i = 0; i < rel_pointer_count; i++) {
        EFI_SIMPLE_POINTER_STATE state;
        rel_pointers[i]->GetState(rel_pointers[i], &state);
    }
    for (size_t i = 0; i < abs_pointer_count; i++) {
        EFI_ABSOLUTE_POINTER_STATE state;
        abs_pointers[i]->GetState(abs_pointers[i], &state);
    }
    discard_first_move = (1 << rel_pointer_count) - 1;
    reset_transient_state();
    seen_released = false;
}

#endif
