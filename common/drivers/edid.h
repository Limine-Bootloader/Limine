#ifndef DRIVERS__EDID_H__
#define DRIVERS__EDID_H__

#include <stdint.h>

struct edid_info_struct {
    uint8_t header[8];
    uint16_t manufacturer_id_be;
    uint16_t edid_id_code;
    uint32_t serial_num;
    uint8_t man_week;
    uint8_t man_year;
    uint8_t edid_version;
    uint8_t edid_revision;
    uint8_t video_input_type;
    uint8_t max_hor_size;
    uint8_t max_ver_size;
    uint8_t gamma_factor;
    uint8_t dpms_flags;
    uint8_t chroma_info[10];
    uint8_t est_timings1;
    uint8_t est_timings2;
    uint8_t man_res_timing;
    uint16_t std_timing_id[8];
    uint8_t det_timing_desc1[18];
    uint8_t det_timing_desc2[18];
    uint8_t det_timing_desc3[18];
    uint8_t det_timing_desc4[18];
    uint8_t unused;
    uint8_t checksum;
} __attribute__((packed));

#if defined (UEFI)
#include <efi.h>

struct edid_info_struct *get_edid_info(EFI_HANDLE gop_handle);
#endif

#if defined (BIOS)
struct edid_info_struct *get_edid_info(void);
#endif

#endif
