#if defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <efi.h>
#include <lib/misc.h>
#include <lib/term.h>
#include <drivers/gop.h>
#include <drivers/edid.h>
#include <lib/print.h>
#include <mm/pmm.h>

static uint16_t linear_masks_to_bpp(uint32_t red_mask, uint32_t green_mask,
                                    uint32_t blue_mask, uint32_t alpha_mask) {
    uint32_t compound_mask = red_mask | green_mask | blue_mask | alpha_mask;
    uint16_t ret = 32;
    while ((compound_mask & (1 << 31)) == 0) {
        ret--;
        compound_mask <<= 1;
    }
    return ret;
}

static void linear_mask_to_mask_shift(
                uint8_t *mask, uint8_t *shift, uint32_t linear_mask) {
    *shift = 0;
    *mask = 0;
    if (linear_mask == 0) {
        return;
    }
    while ((linear_mask & 1) == 0) {
        (*shift)++;
        linear_mask >>= 1;
    }
    while ((linear_mask & 1) == 1) {
        (*mask)++;
        linear_mask >>= 1;
    }
}

static bool validate_pitch(struct fb_info *ret, size_t mode) {
    uint64_t bytes_per_pixel = ret->framebuffer_bpp / 8;
    if (bytes_per_pixel == 0
     || ret->framebuffer_pitch % bytes_per_pixel != 0
     || ret->framebuffer_pitch < ret->framebuffer_width * bytes_per_pixel) {
        printv("gop: Mode %u has invalid pitch %u (width=%u, bpp=%u), skipping.\n",
               (uint32_t)mode, (uint32_t)ret->framebuffer_pitch,
               (uint32_t)ret->framebuffer_width, (uint32_t)ret->framebuffer_bpp);
        return false;
    }
    return true;
}

// Most of this code taken from https://wiki.osdev.org/GOP

static bool mode_to_fb_info(struct fb_info *ret, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, size_t mode) {
    EFI_STATUS status;

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode_info;
    UINTN mode_info_size;

    status = gop->QueryMode(gop, mode, &mode_info_size, &mode_info);

    if (status) {
        return false;
    }

    switch (mode_info->PixelFormat) {
        case PixelBlueGreenRedReserved8BitPerColor:
            ret->framebuffer_bpp = 32;
            ret->red_mask_size = 8;
            ret->red_mask_shift = 16;
            ret->green_mask_size = 8;
            ret->green_mask_shift = 8;
            ret->blue_mask_size = 8;
            ret->blue_mask_shift = 0;
            break;
        case PixelRedGreenBlueReserved8BitPerColor:
            ret->framebuffer_bpp = 32;
            ret->red_mask_size = 8;
            ret->red_mask_shift = 0;
            ret->green_mask_size = 8;
            ret->green_mask_shift = 8;
            ret->blue_mask_size = 8;
            ret->blue_mask_shift = 16;
            break;
        case PixelBitMask:
            if ((mode_info->PixelInformation.RedMask
               | mode_info->PixelInformation.GreenMask
               | mode_info->PixelInformation.BlueMask
               | mode_info->PixelInformation.ReservedMask) == 0) {
                return false;
            }
            ret->framebuffer_bpp = linear_masks_to_bpp(
                                      mode_info->PixelInformation.RedMask,
                                      mode_info->PixelInformation.GreenMask,
                                      mode_info->PixelInformation.BlueMask,
                                      mode_info->PixelInformation.ReservedMask);
            linear_mask_to_mask_shift(&ret->red_mask_size,
                                      &ret->red_mask_shift,
                                      mode_info->PixelInformation.RedMask);
            linear_mask_to_mask_shift(&ret->green_mask_size,
                                      &ret->green_mask_shift,
                                      mode_info->PixelInformation.GreenMask);
            linear_mask_to_mask_shift(&ret->blue_mask_size,
                                      &ret->blue_mask_shift,
                                      mode_info->PixelInformation.BlueMask);
            break;
        default:
            return false;
    }

    ret->memory_model = 0x06;
    ret->framebuffer_pitch = mode_info->PixelsPerScanLine * (ret->framebuffer_bpp / 8);
    ret->framebuffer_width = mode_info->HorizontalResolution;
    ret->framebuffer_height = mode_info->VerticalResolution;

    if (!validate_pitch(ret, mode)) {
        return false;
    }

    return true;
}

bool gop_force_16 = false;

static bool try_mode(struct fb_info *ret, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                     size_t mode, uint64_t width, uint64_t height, int bpp,
                     struct fb_info *fbs, size_t fbs_count,
                     bool preserve_screen) {
    EFI_STATUS status;

    if (!mode_to_fb_info(ret, gop, mode)) {
        return false;
    }

    if (width != 0 && height != 0 && bpp != 0) {
        if (ret->framebuffer_width != width
         || ret->framebuffer_height != height
         || ret->framebuffer_bpp != bpp) {
            return false;
        }
    }

    if (gop_force_16) {
        if (ret->framebuffer_width >= 65536
         || ret->framebuffer_height >= 65536
         || ret->framebuffer_pitch >= 65536) {
            return false;
        }
    }

    for (size_t i = 0; i < fbs_count; i++) {
        if (gop->Mode->FrameBufferBase == fbs[i].framebuffer_addr) {
            return false;
        }
    }

    printv("gop: Found matching mode %X, attempting to set...\n", (uint64_t)mode);

    if (mode == gop->Mode->Mode) {
        printv("gop: Mode was already set, perfect!\n");
    } else {
        status = gop->SetMode(gop, mode);

        if (status) {
            printv("gop: Failed to set video mode %X, moving on...\n", (uint64_t)mode);
            return false;
        }
    }

    // Recalculate pitch from gop->Mode->Info, as some firmware (e.g. Apple
    // Macs) report incorrect PixelsPerScanLine via QueryMode.
    ret->framebuffer_pitch = gop->Mode->Info->PixelsPerScanLine * (ret->framebuffer_bpp / 8);

    if (!validate_pitch(ret, mode)) {
        return false;
    }

    ret->framebuffer_addr = gop->Mode->FrameBufferBase;

    if (!preserve_screen) {
        fb_clear(ret);
    }

    return true;
}

static struct fb_info *get_mode_list(size_t *count, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop) {
    UINTN modes_count = gop->Mode->MaxMode;

    struct fb_info *ret = ext_mem_alloc_counted(modes_count, sizeof(struct fb_info));

    size_t actual_count = 0;
    for (size_t i = 0; i < modes_count; i++) {
        if (mode_to_fb_info(&ret[actual_count], gop, i)) {
            actual_count++;
        }
    }

    struct fb_info *tmp = ext_mem_alloc_counted(actual_count, sizeof(struct fb_info));
    memcpy(tmp, ret, actual_count * sizeof(struct fb_info));

    pmm_free(ret, modes_count * sizeof(struct fb_info));
    ret = tmp;

    *count = actual_count;
    return ret;
}

#define MAX_PRESET_MODES 128
no_unwind static int preset_modes[MAX_PRESET_MODES];
no_unwind static bool preset_modes_initialised = false;

void init_gop(struct fb_info **ret, size_t *_fbs_count,
              uint64_t target_width, uint64_t target_height, uint16_t target_bpp,
              bool preserve_screen) {
    if (preset_modes_initialised == false) {
        for (size_t i = 0; i < MAX_PRESET_MODES; i++) {
            preset_modes[i] = -1;
        }
        preset_modes_initialised = true;
    }

    EFI_STATUS status;

    EFI_HANDLE tmp_handles[1];

    EFI_HANDLE *handles = tmp_handles;
    UINTN handles_size = sizeof(EFI_HANDLE);
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    status = gBS->LocateHandle(ByProtocol, &gop_guid, NULL, &handles_size, handles);

    if (status != EFI_SUCCESS && status != EFI_BUFFER_TOO_SMALL) {
        *_fbs_count = 0;
        return;
    }

    handles = ext_mem_alloc(handles_size);

    status = gBS->LocateHandle(ByProtocol, &gop_guid, NULL, &handles_size, handles);
    if (status != EFI_SUCCESS) {
        pmm_free(handles, handles_size);
        *_fbs_count = 0;
        return;
    }

    size_t handles_count = handles_size / sizeof(EFI_HANDLE);

    *ret = ext_mem_alloc_counted(handles_count, sizeof(struct fb_info));

    const struct resolution fallback_resolutions[] = {
        { 0,    0,   0  },   // Overridden by EDID
        { 0,    0,   0  },   // Overridden by preset
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

    size_t fbs_count = 0;
    for (size_t i = 0; i < handles_count && i < MAX_PRESET_MODES; i++) {
        struct fb_info *fb = &(*ret)[fbs_count];

        uint64_t _target_width = target_width;
        uint64_t _target_height = target_height;
        uint64_t _target_bpp = target_bpp;

        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;

        status = gBS->HandleProtocol(handles[i], &gop_guid, (void **)&gop);
        if (status != EFI_SUCCESS) {
            continue;
        }

        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode_info;
        UINTN mode_info_size;

        status = gop->QueryMode(gop, gop->Mode == NULL ? 0 : gop->Mode->Mode,
                                &mode_info_size, &mode_info);

        if (status == EFI_NOT_STARTED) {
            if (fbs_count > 0) {
                continue;
            }
            status = gop->SetMode(gop, 0);
            if (status) {
                continue;
            }
            status = gop->QueryMode(gop, gop->Mode == NULL ? 0 : gop->Mode->Mode,
                                    &mode_info_size, &mode_info);
        }

        if (status) {
            continue;
        }

        if (preset_modes[i] == -1) {
            preset_modes[i] = gop->Mode->Mode;
        }

        fb->edid = get_edid_info(handles[i]);

        UINTN modes_count = gop->Mode->MaxMode;

        size_t current_fallback = 0;

        if (!_target_width || !_target_height || !_target_bpp) {
            goto fallback;
        } else {
            printv("gop: Requested resolution of %ux%ux%u\n",
                   _target_width, _target_height, _target_bpp);
        }

retry:
        for (size_t j = 0; j < modes_count; j++) {
            if (try_mode(fb, gop, j, _target_width, _target_height, _target_bpp, *ret, fbs_count, preserve_screen)) {
                goto success;
            }
        }

fallback:
        if (current_fallback == 0) {
            current_fallback++;

            if (fb->edid != NULL) {
                uint64_t edid_width = (uint64_t)fb->edid->det_timing_desc1[2];
                         edid_width += ((uint64_t)fb->edid->det_timing_desc1[4] & 0xf0) << 4;
                uint64_t edid_height = (uint64_t)fb->edid->det_timing_desc1[5];
                         edid_height += ((uint64_t)fb->edid->det_timing_desc1[7] & 0xf0) << 4;
                if (edid_width >= mode_info->HorizontalResolution
                 && edid_height >= mode_info->VerticalResolution) {
                    _target_width = edid_width;
                    _target_height = edid_height;
                    _target_bpp = 32;
                    goto retry;
                }
            }
        }

        if (current_fallback == 1) {
            current_fallback++;

            if (try_mode(fb, gop, preset_modes[i], 0, 0, 0, *ret, fbs_count, preserve_screen)) {
                goto success;
            }
        }

        if (current_fallback < SIZEOF_ARRAY(fallback_resolutions)) {
            _target_width = fallback_resolutions[current_fallback].width;
            _target_height = fallback_resolutions[current_fallback].height;
            _target_bpp = fallback_resolutions[current_fallback].bpp;

            current_fallback++;
            goto retry;
        }

        continue;

success:;
        size_t mode_count;
        fb->mode_list = get_mode_list(&mode_count, gop);
        fb->mode_count = mode_count;

        fbs_count++;
    }

    pmm_free(handles, handles_size);

    gop_force_16 = false;

    *_fbs_count = fbs_count;
}

#endif
