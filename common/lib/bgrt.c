#include <stddef.h>
#include <stdint.h>
#include <lib/bgrt.h>
#include <lib/acpi.h>
#include <lib/config.h>
#include <lib/libc.h>
#include <lib/misc.h>

#if defined (UEFI)

void bgrt_restore(uint64_t fb_width, uint64_t fb_height) {
    if (!firmware_logo) {
        return;
    }

    struct acpi_bgrt *bgrt = acpi_get_table("BGRT", 0);
    if (bgrt == NULL) {
        return;
    }

    if (bgrt->header.length < sizeof(struct acpi_bgrt)) {
        return;
    }

    if (bgrt->version != 1) {
        return;
    }

    if (bgrt->image_address > UINTPTR_MAX) {
        return;
    }

    bgrt->status &= ~1;

    uint8_t *bmp = (uint8_t *)(uintptr_t)bgrt->image_address;
    if (bmp != NULL && bmp[0] == 'B' && bmp[1] == 'M') {
        // Firmware BMP size field is unreliable, bounds check is not possible.
        // The scope is limited to 4 bytes; we choose to trust the firmware here.
        uint32_t dib_header_size;
        memcpy(&dib_header_size, &bmp[14], sizeof(dib_header_size));

        uint32_t bmp_width = 0;
        uint32_t bmp_height = 0;

        if (dib_header_size >= 40) {
            int32_t bmp_height_raw;
            memcpy(&bmp_width, &bmp[18], sizeof(bmp_width));
            memcpy(&bmp_height_raw, &bmp[22], sizeof(bmp_height_raw));

            bmp_height = (uint32_t)(bmp_height_raw < 0 ? -bmp_height_raw : bmp_height_raw);
        } else if (dib_header_size == 12) {
            uint16_t bmp_width_raw;
            uint16_t bmp_height_raw;
            memcpy(&bmp_width_raw, &bmp[18], sizeof(bmp_width_raw));
            memcpy(&bmp_height_raw, &bmp[20], sizeof(bmp_height_raw));

            bmp_width = bmp_width_raw;
            bmp_height = bmp_height_raw;
        }

        if (bmp_width > 0 && bmp_height > 0) {
            if (fb_width >= bmp_width) {
                bgrt->image_offset_x = (fb_width - bmp_width) / 2;
            }

            if (fb_height >= bmp_height) {
                bgrt->image_offset_y = (fb_height - bmp_height) / 2;
            }
        }
    }

    bgrt->header.checksum = 0;
    bgrt->header.checksum = 256 - acpi_checksum(bgrt, bgrt->header.length);
}

#else

void bgrt_restore(uint64_t fb_width, uint64_t fb_height) {
    (void)fb_width;
    (void)fb_height;
}

#endif
