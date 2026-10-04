#ifndef PROTOS__LINUX_H__
#define PROTOS__LINUX_H__

#include <stddef.h>
#include <stdint.h>
#include <stdnoreturn.h>

// The first member of the x86 zero page; non-x86 kernels get a copy through an
// EFI configuration table. Fields Limine does not fill must stay zero.
struct screen_info {
    uint8_t reserved_00[6];
    uint8_t orig_video_mode;
    uint8_t orig_video_cols;
    uint8_t flags;
    uint8_t reserved_09[1];
    uint16_t orig_video_ega_bx;
    uint8_t reserved_0c[2];
    uint8_t orig_video_lines;
    uint8_t orig_video_isVGA;
    uint16_t orig_video_points;
    uint16_t lfb_width;
    uint16_t lfb_height;
    uint16_t lfb_depth;
    uint32_t lfb_base;
    uint32_t lfb_size;
    uint8_t reserved_20[4];
    uint16_t lfb_linelength;
    uint8_t red_size;
    uint8_t red_pos;
    uint8_t green_size;
    uint8_t green_pos;
    uint8_t blue_size;
    uint8_t blue_pos;
    uint8_t reserved_2c[10];
    uint32_t capabilities;
    uint32_t ext_lfb_base;
    uint8_t reserved_3e[2];
} __attribute__((packed));

_Static_assert(offsetof(struct screen_info, orig_video_mode) == 0x06, "screen_info layout");
_Static_assert(offsetof(struct screen_info, orig_video_cols) == 0x07, "screen_info layout");
_Static_assert(offsetof(struct screen_info, flags) == 0x08, "screen_info layout");
_Static_assert(offsetof(struct screen_info, orig_video_ega_bx) == 0x0a, "screen_info layout");
_Static_assert(offsetof(struct screen_info, orig_video_lines) == 0x0e, "screen_info layout");
_Static_assert(offsetof(struct screen_info, orig_video_isVGA) == 0x0f, "screen_info layout");
_Static_assert(offsetof(struct screen_info, orig_video_points) == 0x10, "screen_info layout");
_Static_assert(offsetof(struct screen_info, lfb_width) == 0x12, "screen_info layout");
_Static_assert(offsetof(struct screen_info, lfb_height) == 0x14, "screen_info layout");
_Static_assert(offsetof(struct screen_info, lfb_depth) == 0x16, "screen_info layout");
_Static_assert(offsetof(struct screen_info, lfb_base) == 0x18, "screen_info layout");
_Static_assert(offsetof(struct screen_info, lfb_size) == 0x1c, "screen_info layout");
_Static_assert(offsetof(struct screen_info, lfb_linelength) == 0x24, "screen_info layout");
_Static_assert(offsetof(struct screen_info, red_size) == 0x26, "screen_info layout");
_Static_assert(offsetof(struct screen_info, red_pos) == 0x27, "screen_info layout");
_Static_assert(offsetof(struct screen_info, green_size) == 0x28, "screen_info layout");
_Static_assert(offsetof(struct screen_info, green_pos) == 0x29, "screen_info layout");
_Static_assert(offsetof(struct screen_info, blue_size) == 0x2a, "screen_info layout");
_Static_assert(offsetof(struct screen_info, blue_pos) == 0x2b, "screen_info layout");
_Static_assert(offsetof(struct screen_info, capabilities) == 0x36, "screen_info layout");
_Static_assert(offsetof(struct screen_info, ext_lfb_base) == 0x3a, "screen_info layout");
_Static_assert(sizeof(struct screen_info) == 0x40, "screen_info layout");

// Values of orig_video_isVGA.
#define VIDEO_TYPE_VGAC 0x22
#define VIDEO_TYPE_VLFB 0x23
#define VIDEO_TYPE_EFI 0x70

#define VIDEO_FLAGS_NOCURSOR (1 << 0)

// Set when ext_lfb_base holds the upper half of the framebuffer address.
#define VIDEO_CAPABILITY_64BIT_BASE (1 << 1)

noreturn void linux_load(char *config, char *cmdline);

#if defined (UEFI)
void linux_install_efi_tpm_event_log(void);
#endif

#endif
