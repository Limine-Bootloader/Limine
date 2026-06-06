#if defined (BIOS)

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <drivers/vbe.h>
#include <drivers/edid.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/real.h>
#include <lib/print.h>
#include <lib/image.h>
#include <lib/config.h>
#include <lib/uri.h>
#include <lib/term.h>
#include <mm/pmm.h>

struct vbe_info_struct {
    char     signature[4];
    uint8_t  version_min;
    uint8_t  version_maj;
    uint16_t oem_off;
    uint16_t oem_seg;
    uint32_t capabilities;
    uint16_t vid_modes_off;
    uint16_t vid_modes_seg;
    uint16_t vid_mem_blocks;
    uint16_t software_rev;
    uint16_t vendor_off;
    uint16_t vendor_seg;
    uint16_t prod_name_off;
    uint16_t prod_name_seg;
    uint16_t prod_rev_off;
    uint16_t prod_rev_seg;
    uint8_t  reserved[222];
    uint8_t  oem_data[256];
} __attribute__((packed));

struct vbe_mode_info_struct {
    uint16_t mode_attributes;
    uint8_t  wina_attributes;
    uint8_t  winb_attributes;
    uint16_t win_granularity;
    uint16_t win_size;
    uint16_t wina_segment;
    uint16_t winb_segment;
    uint32_t win_farptr;
    uint16_t bytes_per_scanline;

    uint16_t res_x;
    uint16_t res_y;
    uint8_t  charsize_x;
    uint8_t  charsize_y;
    uint8_t  plane_count;
    uint8_t  bpp;
    uint8_t  bank_count;
    uint8_t  memory_model;
    uint8_t  bank_size;
    uint8_t  image_count;
    uint8_t  reserved0;

    uint8_t  red_mask_size;
    uint8_t  red_mask_shift;
    uint8_t  green_mask_size;
    uint8_t  green_mask_shift;
    uint8_t  blue_mask_size;
    uint8_t  blue_mask_shift;
    uint8_t  rsvd_mask_size;
    uint8_t  rsvd_mask_shift;
    uint8_t  direct_color_info;

    uint32_t framebuffer_addr;
    uint8_t  reserved1[6];

    uint16_t lin_bytes_per_scanline;
    uint8_t  banked_image_count;
    uint8_t  lin_image_count;
    uint8_t  lin_red_mask_size;
    uint8_t  lin_red_mask_shift;
    uint8_t  lin_green_mask_size;
    uint8_t  lin_green_mask_shift;
    uint8_t  lin_blue_mask_size;
    uint8_t  lin_blue_mask_shift;
    uint8_t  lin_rsvd_mask_size;
    uint8_t  lin_rsvd_mask_shift;
    uint32_t max_pixel_clock;

    uint8_t  reserved2[190];
} __attribute__((packed));

static bool get_vbe_info(struct vbe_info_struct *buf) {
    struct rm_regs r = {0};

    memcpy(buf->signature, "VBE2", 4);

    r.eax = 0x4f00;
    r.edi = (uint32_t)buf;
    rm_int(0x10, &r, &r);

    if ((r.eax & 0xff00) >> 8 != 0
     || (r.eax & 0x00ff) != 0x4f) {
        return false;
    }

    if (memcmp(buf->signature, "VESA", 4) != 0) {
        printv("vbe: warning: missing VESA signature in info block\n");
    }

    return true;
}

static bool get_vbe_mode_info(struct vbe_mode_info_struct *buf,
                              uint16_t mode) {
    struct rm_regs r = {0};

    r.eax = 0x4f01;
    r.ecx = (uint32_t)mode;
    r.edi = (uint32_t)buf;
    rm_int(0x10, &r, &r);

    if ((r.eax & 0xff00) >> 8 != 0
     || (r.eax & 0x00ff) != 0x4f) {
        return false;
    }

    return true;
}

static bool set_vbe_mode(uint16_t mode) {
    struct rm_regs r = {0};

    r.eax = 0x4f02;
    r.ebx = (uint32_t)mode | (1 << 14);
    rm_int(0x10, &r, &r);

    if ((r.eax & 0xff00) >> 8 != 0
     || (r.eax & 0x00ff) != 0x4f) {
        return false;
    }

    return true;
}

// Maximum number of video modes to enumerate to prevent infinite loops
// from corrupted VBE mode lists without a proper 0xffff terminator
#define VBE_MAX_MODES 512

struct fb_info *vbe_get_mode_list(size_t *count) {
    struct vbe_info_struct vbe_info;
    if (!get_vbe_info(&vbe_info)) {
        return NULL;
    }

    uint16_t *vid_modes = (uint16_t *)rm_desegment(vbe_info.vid_modes_seg,
                                                   vbe_info.vid_modes_off);

    size_t modes_count = 0;
    for (size_t i = 0; i < VBE_MAX_MODES && vid_modes[i] != 0xffff; i++) {
        struct vbe_mode_info_struct vbe_mode_info;
        if (!get_vbe_mode_info(&vbe_mode_info, vid_modes[i])) {
            continue;
        }

        // We only support RGB for now
        if (vbe_mode_info.memory_model != 0x06)
            continue;
        // We only support linear modes
        if (!(vbe_mode_info.mode_attributes & (1 << 7)))
            continue;

        uint16_t pitch = (vbe_info.version_maj < 3)
                       ? vbe_mode_info.bytes_per_scanline
                       : vbe_mode_info.lin_bytes_per_scanline;
        uint16_t bytes_per_pixel = vbe_mode_info.bpp / 8;
        if (bytes_per_pixel == 0
         || pitch % bytes_per_pixel != 0
         || pitch < (uint32_t)vbe_mode_info.res_x * bytes_per_pixel)
            continue;

        modes_count++;
    }

    struct fb_info *ret = ext_mem_alloc_counted(modes_count, sizeof(struct fb_info));

    for (size_t i = 0, j = 0; i < VBE_MAX_MODES && vid_modes[i] != 0xffff; i++) {
        struct vbe_mode_info_struct vbe_mode_info;
        if (!get_vbe_mode_info(&vbe_mode_info, vid_modes[i])) {
            continue;
        }

        // We only support RGB for now
        if (vbe_mode_info.memory_model != 0x06)
            continue;
        // We only support linear modes
        if (!(vbe_mode_info.mode_attributes & (1 << 7)))
            continue;

        uint16_t pitch = (vbe_info.version_maj < 3)
                       ? vbe_mode_info.bytes_per_scanline
                       : vbe_mode_info.lin_bytes_per_scanline;
        uint16_t bytes_per_pixel = vbe_mode_info.bpp / 8;
        if (bytes_per_pixel == 0
         || pitch % bytes_per_pixel != 0
         || pitch < (uint32_t)vbe_mode_info.res_x * bytes_per_pixel)
            continue;

        ret[j].memory_model = vbe_mode_info.memory_model;

        ret[j].framebuffer_width = vbe_mode_info.res_x;
        ret[j].framebuffer_height = vbe_mode_info.res_y;
        ret[j].framebuffer_bpp = vbe_mode_info.bpp;

        if (vbe_info.version_maj < 3) {
            ret[j].framebuffer_pitch  = vbe_mode_info.bytes_per_scanline;
            ret[j].red_mask_size      = vbe_mode_info.red_mask_size;
            ret[j].red_mask_shift     = vbe_mode_info.red_mask_shift;
            ret[j].green_mask_size    = vbe_mode_info.green_mask_size;
            ret[j].green_mask_shift   = vbe_mode_info.green_mask_shift;
            ret[j].blue_mask_size     = vbe_mode_info.blue_mask_size;
            ret[j].blue_mask_shift    = vbe_mode_info.blue_mask_shift;
        } else {
            ret[j].framebuffer_pitch  = vbe_mode_info.lin_bytes_per_scanline;
            ret[j].red_mask_size      = vbe_mode_info.lin_red_mask_size;
            ret[j].red_mask_shift     = vbe_mode_info.lin_red_mask_shift;
            ret[j].green_mask_size    = vbe_mode_info.lin_green_mask_size;
            ret[j].green_mask_shift   = vbe_mode_info.lin_green_mask_shift;
            ret[j].blue_mask_size     = vbe_mode_info.lin_blue_mask_size;
            ret[j].blue_mask_shift    = vbe_mode_info.lin_blue_mask_shift;
        }

        j++;
    }

    *count = modes_count;

    return ret;
}

bool init_vbe(struct fb_info *ret,
              uint16_t target_width, uint16_t target_height, uint16_t target_bpp) {
    printv("vbe: Initialising...\n");

    size_t current_fallback = 0;

    struct vbe_info_struct vbe_info;
    if (!get_vbe_info(&vbe_info)) {
        return false;
    }

    printv("vbe: Version: %u.%u\n", vbe_info.version_maj, vbe_info.version_min);
    printv("vbe: OEM: %s\n", (char *)rm_desegment(vbe_info.oem_seg, vbe_info.oem_off));
    printv("vbe: Graphics vendor: %s\n", (char *)rm_desegment(vbe_info.vendor_seg, vbe_info.vendor_off));
    printv("vbe: Product name: %s\n", (char *)rm_desegment(vbe_info.prod_name_seg, vbe_info.prod_name_off));
    printv("vbe: Product revision: %s\n", (char *)rm_desegment(vbe_info.prod_rev_seg, vbe_info.prod_rev_off));

    uint16_t *vid_modes = (uint16_t *)rm_desegment(vbe_info.vid_modes_seg,
                                                   vbe_info.vid_modes_off);

    struct resolution fallback_resolutions[] = {
        { 1024, 768, 32 },
        { 800,  600, 32 },
        { 640,  480, 32 },
        { 1024, 768, 24 },
        { 800,  600, 24 },
        { 640,  480, 24 },
        { 1024, 768, 16 },
        { 800,  600, 16 },
        { 640,  480, 16 }
    };

    if (!target_width || !target_height || !target_bpp) {
        struct edid_info_struct *edid_info = get_edid_info();
        if (edid_info != NULL) {
            int edid_width   = (int)edid_info->det_timing_desc1[2];
                edid_width  += ((int)edid_info->det_timing_desc1[4] & 0xf0) << 4;
            int edid_height  = (int)edid_info->det_timing_desc1[5];
                edid_height += ((int)edid_info->det_timing_desc1[7] & 0xf0) << 4;
            if (edid_width && edid_height) {
                target_width  = edid_width;
                target_height = edid_height;
                target_bpp    = 32;
                printv("vbe: EDID detected screen resolution of %ux%u\n",
                       target_width, target_height);
                goto retry;
            }
        }
        goto fallback;
    } else {
        printv("vbe: Requested resolution of %ux%ux%u\n",
               target_width, target_height, target_bpp);
    }

retry:
    for (size_t i = 0; i < VBE_MAX_MODES && vid_modes[i] != 0xffff; i++) {
        struct vbe_mode_info_struct vbe_mode_info;
        if (!get_vbe_mode_info(&vbe_mode_info, vid_modes[i])) {
            continue;
        }
        if  (vbe_mode_info.res_x == target_width
          && vbe_mode_info.res_y == target_height
          && vbe_mode_info.bpp   == target_bpp) {
            // We only support RGB for now
            if (vbe_mode_info.memory_model != 0x06)
                continue;
            // We only support linear modes
            if (!(vbe_mode_info.mode_attributes & (1 << 7)))
                continue;
            printv("vbe: Found matching mode %x, attempting to set...\n", vid_modes[i]);
            if (vid_modes[i] == current_video_mode) {
                printv("vbe: Mode was already set, perfect!\n");
            } else if (!set_vbe_mode(vid_modes[i])) {
                current_video_mode = -1;
                printv("vbe: Failed to set video mode %x, moving on...\n", vid_modes[i]);
                continue;
            }
            current_video_mode = vid_modes[i];

            printv("vbe: Framebuffer address: %x\n", vbe_mode_info.framebuffer_addr);
            ret->memory_model       = vbe_mode_info.memory_model;
            ret->framebuffer_addr   = vbe_mode_info.framebuffer_addr;
            ret->framebuffer_width  = vbe_mode_info.res_x;
            ret->framebuffer_height = vbe_mode_info.res_y;
            ret->framebuffer_bpp    = vbe_mode_info.bpp;
            if (vbe_info.version_maj < 3) {
                ret->framebuffer_pitch  = vbe_mode_info.bytes_per_scanline;
                ret->red_mask_size      = vbe_mode_info.red_mask_size;
                ret->red_mask_shift     = vbe_mode_info.red_mask_shift;
                ret->green_mask_size    = vbe_mode_info.green_mask_size;
                ret->green_mask_shift   = vbe_mode_info.green_mask_shift;
                ret->blue_mask_size     = vbe_mode_info.blue_mask_size;
                ret->blue_mask_shift    = vbe_mode_info.blue_mask_shift;
            } else {
                ret->framebuffer_pitch  = vbe_mode_info.lin_bytes_per_scanline;
                ret->red_mask_size      = vbe_mode_info.lin_red_mask_size;
                ret->red_mask_shift     = vbe_mode_info.lin_red_mask_shift;
                ret->green_mask_size    = vbe_mode_info.lin_green_mask_size;
                ret->green_mask_shift   = vbe_mode_info.lin_green_mask_shift;
                ret->blue_mask_size     = vbe_mode_info.lin_blue_mask_size;
                ret->blue_mask_shift    = vbe_mode_info.lin_blue_mask_shift;
            }

            uint16_t bytes_per_pixel = ret->framebuffer_bpp / 8;
            if (bytes_per_pixel == 0
             || ret->framebuffer_pitch % bytes_per_pixel != 0
             || ret->framebuffer_pitch < (uint32_t)ret->framebuffer_width * bytes_per_pixel) {
                printv("vbe: Mode %x has invalid pitch %u (width=%u, bpp=%u), skipping.\n",
                       vid_modes[i], (uint32_t)ret->framebuffer_pitch,
                       (uint32_t)ret->framebuffer_width, (uint32_t)ret->framebuffer_bpp);
                continue;
            }

            return true;
        }
    }

fallback:
    if (current_fallback < SIZEOF_ARRAY(fallback_resolutions)) {
        target_width  = fallback_resolutions[current_fallback].width;
        target_height = fallback_resolutions[current_fallback].height;
        target_bpp    = fallback_resolutions[current_fallback].bpp;
        current_fallback++;
        goto retry;
    }

    return false;
}

#endif
