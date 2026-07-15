#ifndef DRIVERS__MOUSE_H__
#define DRIVERS__MOUSE_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#if defined (UEFI)
#  include <efi.h>
#endif

#define MOUSE_EVENT_MOVE (1 << 0)
#define MOUSE_EVENT_BUTTON (1 << 1)
#define MOUSE_EVENT_WHEEL (1 << 2)

struct mouse_state {
    size_t x, y; // Pointer position.
    // Accumulated wheel steps since the last mouse_get_state() call.
    // Positive => scrolling down.
    int wheel;
    // The pointer moved to a different cell since the last
    // mouse_get_state() call.
    bool moved;
    // Full left button press+release pair happened since the last
    // flush, with both edges on the same row.
    bool click;
    size_t click_x, click_y;
};

bool mouse_init(void);
void mouse_deinit(void);
bool mouse_present(void);
bool mouse_state_pending(void);
void mouse_flush(void);
void mouse_get_state(struct mouse_state *state);
void mouse_render_pointer(void);
void mouse_render_pointer_overlay(void);
void mouse_erase_pointer(void);

#if defined (BIOS)
int mouse_process_pending(void);
#elif defined (UEFI)
size_t mouse_get_efi_events(EFI_EVENT *events, size_t max);
int mouse_handle_efi_event(size_t index);
#endif

#endif
