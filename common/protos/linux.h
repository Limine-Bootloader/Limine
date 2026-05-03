#ifndef PROTOS__LINUX_H__
#define PROTOS__LINUX_H__

#include <stdnoreturn.h>

struct screen_info {
    uint8_t  orig_x;        /* 0x00 */
    uint8_t  orig_y;        /* 0x01 */
    uint16_t ext_mem_k;    /* 0x02 */
    uint16_t orig_video_page;    /* 0x04 */
    uint8_t  orig_video_mode;    /* 0x06 */
    uint8_t  orig_video_cols;    /* 0x07 */
    uint8_t  flags;        /* 0x08 */
    uint8_t  unused2;        /* 0x09 */
    uint16_t orig_video_ega_bx;/* 0x0a */
    uint16_t unused3;        /* 0x0c */
    uint8_t  orig_video_lines;    /* 0x0e */
    uint8_t  orig_video_isVGA;    /* 0x0f */
    uint16_t orig_video_points;/* 0x10 */

    /* VESA graphic mode -- linear frame buffer */
    uint16_t lfb_width;    /* 0x12 */
    uint16_t lfb_height;    /* 0x14 */
    uint16_t lfb_depth;    /* 0x16 */
    uint32_t lfb_base;        /* 0x18 */
    uint32_t lfb_size;        /* 0x1c */
    uint16_t cl_magic, cl_offset; /* 0x20 */
    uint16_t lfb_linelength;    /* 0x24 */
    uint8_t  red_size;        /* 0x26 */
    uint8_t  red_pos;        /* 0x27 */
    uint8_t  green_size;    /* 0x28 */
    uint8_t  green_pos;    /* 0x29 */
    uint8_t  blue_size;    /* 0x2a */
    uint8_t  blue_pos;        /* 0x2b */
    uint8_t  rsvd_size;    /* 0x2c */
    uint8_t  rsvd_pos;        /* 0x2d */
    uint16_t vesapm_seg;    /* 0x2e */
    uint16_t vesapm_off;    /* 0x30 */
    uint16_t pages;        /* 0x32 */
    uint16_t vesa_attributes;    /* 0x34 */
    uint32_t capabilities;     /* 0x36 */
    uint32_t ext_lfb_base;    /* 0x3a */
    uint8_t  _reserved[2];    /* 0x3e */
} __attribute__((packed));

#define VIDEO_TYPE_MDA        0x10    /* Monochrome Text Display    */
#define VIDEO_TYPE_CGA        0x11    /* CGA Display             */
#define VIDEO_TYPE_EGAM        0x20    /* EGA/VGA in Monochrome Mode    */
#define VIDEO_TYPE_EGAC        0x21    /* EGA in Color Mode        */
#define VIDEO_TYPE_VGAC        0x22    /* VGA+ in Color Mode        */
#define VIDEO_TYPE_VLFB        0x23    /* VESA VGA in graphic mode    */

#define VIDEO_TYPE_PICA_S3    0x30    /* ACER PICA-61 local S3 video    */
#define VIDEO_TYPE_MIPS_G364    0x31    /* MIPS Magnum 4000 G364 video  */
#define VIDEO_TYPE_SGI          0x33    /* Various SGI graphics hardware */

#define VIDEO_TYPE_TGAC        0x40    /* DEC TGA */

#define VIDEO_TYPE_SUN          0x50    /* Sun frame buffer. */
#define VIDEO_TYPE_SUNPCI       0x51    /* Sun PCI based frame buffer. */

#define VIDEO_TYPE_PMAC        0x60    /* PowerMacintosh frame buffer. */

#define VIDEO_TYPE_EFI        0x70    /* EFI graphic mode        */

#define VIDEO_FLAGS_NOCURSOR    (1 << 0) /* The video mode has no cursor set */

#define VIDEO_CAPABILITY_SKIP_QUIRKS    (1 << 0)
#define VIDEO_CAPABILITY_64BIT_BASE    (1 << 1)    /* Frame buffer base is 64-bit */

noreturn void linux_load(char *config, char *cmdline);

#if defined (UEFI)
void linux_install_efi_tpm_event_log(void);
#endif

#endif
