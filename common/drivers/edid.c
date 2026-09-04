#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <drivers/gop.h>
#include <drivers/edid.h>
#include <mm/pmm.h>
#include <lib/misc.h>
#include <lib/libc.h>
#include <lib/print.h>

// VESA E-EDID A2: the first eight bytes are a fixed pattern (3.3), and the
// reader is required to check that the whole 128-byte block sums to zero
// (3.11, note 3).
static bool edid_is_valid(const struct edid_info_struct *edid) {
    _Static_assert(sizeof(struct edid_info_struct) == 128,
                   "EDID base block is 128 bytes");

    static const uint8_t magic[8] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
    };

    if (memcmp(edid->header, magic, sizeof(magic)) != 0) {
        return false;
    }

    uint8_t sum = 0;
    for (size_t i = 0; i < sizeof(struct edid_info_struct); i++) {
        sum += ((const uint8_t *)edid)[i];
    }

    return sum == 0;
}

#if defined (BIOS)

#include <lib/real.h>

struct edid_info_struct *get_edid_info(void) {
    static struct edid_info_struct *buf = NULL;

    if (buf == NULL)
        buf = conv_mem_alloc(sizeof(struct edid_info_struct));

    struct rm_regs r = {0};

    r.eax = 0x4f15;
    r.ebx = 0x0001;
    r.edi = (uint32_t)rm_off(buf);
    r.ds  = (uint32_t)rm_seg(buf);
    r.es  = r.ds;
    rm_int(0x10, &r, &r);

    if ((r.eax & 0x00ff) != 0x4f)
        goto fail;
    if ((r.eax & 0xff00) != 0)
        goto fail;

    if (edid_is_valid(buf))
        goto success;

fail:
    printv("edid: No valid EDID data.\n");
    return NULL;

success:
    printv("edid: Success.\n");
    return buf;
}

#endif

#if defined (UEFI)

#include <efi.h>

struct edid_info_struct *get_edid_info(EFI_HANDLE gop_handle) {
    struct edid_info_struct *buf = ext_mem_alloc(sizeof(struct edid_info_struct));

    EFI_STATUS status;

    EFI_EDID_ACTIVE_PROTOCOL *edid = NULL;
    EFI_GUID edid_guid = EFI_EDID_ACTIVE_PROTOCOL_GUID;

    status = gBS->HandleProtocol(gop_handle, &edid_guid, (void **)&edid);

    if (status)
        goto fail;

    if (edid->SizeOfEdid < sizeof(struct edid_info_struct))
        goto fail;

    memcpy(buf, edid->Edid, sizeof(struct edid_info_struct));

    if (edid_is_valid(buf))
        goto success;

fail:
    pmm_free(buf, sizeof(struct edid_info_struct));
    printv("edid: No valid EDID data.\n");
    return NULL;

success:
    printv("edid: Success.\n");
    return buf;
}

#endif
