#include <stdint.h>
#include <stddef.h>
#include <stdalign.h>
#include <drivers/disk.h>
#include <fs/file.h>
#include <lib/libc.h>
#if defined(BIOS)
#  include <lib/real.h>
#elif defined(UEFI)
#  include <efi.h>
#  include <crypt/blake2b.h>
#endif
#include <lib/misc.h>
#include <lib/print.h>
#include <lib/rand.h>
#include <mm/pmm.h>
#include <sys/cpu.h>
#include <pxe/pxe.h>

#define DEFAULT_FASTEST_XFER_SIZE 64
#define MAX_FASTEST_XFER_SIZE 512

#if defined(BIOS)

struct dpte {
    uint16_t io_port;
    uint16_t control_port;
    uint8_t head_reg_upper;
    uint8_t bios_vendor_specific;
    uint8_t irq_info;
    uint8_t block_count_multiple;
    uint8_t dma_info;
    uint8_t pio_info;
    uint16_t flags;
    uint16_t reserved;
    uint8_t revision;
    uint8_t checksum;
} __attribute__((packed));

struct bios_drive_params {
    uint16_t buf_size;
    uint16_t info_flags;
    uint32_t cyl;
    uint32_t heads;
    uint32_t sects;
    uint64_t lba_count;
    uint16_t bytes_per_sect;
    uint16_t dpte_off;
    uint16_t dpte_seg;
} __attribute__((packed));

struct dap {
    uint16_t size;
    uint16_t count;
    uint16_t offset;
    uint16_t segment;
    uint64_t lba;
};

#define XFER_BUF_SIZE (xfer_sizes[SIZEOF_ARRAY(xfer_sizes) - 1] * 512)
static const size_t xfer_sizes[] = { 1, 2, 4, 8, 16, 24, 32, 48, 64 };
static uint8_t *xfer_buf = NULL;

// Some BIOSes fail a transfer that crosses a 64KiB physical boundary (EDD
// information flags bit 0 clear), so a buffer that does is passed over.
static uint8_t *xfer_buf_alloc(void) {
    for (;;) {
        uint8_t *buf = conv_mem_alloc(XFER_BUF_SIZE);

        if ((uintptr_t)buf / 0x10000 == ((uintptr_t)buf + XFER_BUF_SIZE - 1) / 0x10000) {
            return buf;
        }
    }
}

static size_t fastest_xfer_size(struct volume *volume) {
    struct dap dap = {0};

    if (xfer_buf == NULL) {
        xfer_buf = xfer_buf_alloc();
    }

    size_t fastest_size = 1;
    uint64_t last_speed = (uint64_t)-1;

    for (size_t i = 0; i < SIZEOF_ARRAY(xfer_sizes); i++) {
        if (xfer_sizes[i] * volume->sector_size > XFER_BUF_SIZE) {
            break;
        }

        dap.size    = 16;
        dap.count   = xfer_sizes[i];
        dap.segment = rm_seg(xfer_buf);
        dap.offset  = rm_off(xfer_buf);
        dap.lba     = 0;

        uint64_t start_timestamp = rdtsc();
        for (size_t j = 0; j < XFER_BUF_SIZE / 512; j += xfer_sizes[i]) {
            struct rm_regs r = {0};
            r.eax = 0x4200;
            r.edx = volume->drive;
            r.esi = (uint32_t)rm_off(&dap);
            r.ds  = rm_seg(&dap);
            rm_int(0x13, &r, &r);
            if (r.eflags & EFLAGS_CF) {
                int ah = (r.eax >> 8) & 0xff;
                print("Disk error %x. Drive %x", ah, volume->drive);
                return 8;
            }
            dap.lba += xfer_sizes[i];
        }
        uint64_t end_timestamp = rdtsc();

        uint64_t speed = end_timestamp - start_timestamp;

        if (speed < last_speed) {
            last_speed = speed;
            fastest_size = xfer_sizes[i];
        }
    }

    return fastest_size;
}

int disk_read_sectors(struct volume *volume, void *buf, uint64_t block, size_t count) {
    struct dap dap = {0};

    if (count * volume->sector_size > XFER_BUF_SIZE)
        panic(false, "XFER");

    if (xfer_buf == NULL) {
        xfer_buf = xfer_buf_alloc();
    }

    dap.size    = 16;
    dap.count   = count;
    dap.segment = rm_seg(xfer_buf);
    dap.offset  = rm_off(xfer_buf);
    dap.lba     = block;

    struct rm_regs r = {0};
    r.eax = 0x4200;
    r.edx = volume->drive;
    r.esi = (uint32_t)rm_off(&dap);
    r.ds  = rm_seg(&dap);

    rm_int(0x13, &r, &r);

    if (r.eflags & EFLAGS_CF) {
        return DISK_FAILURE;
    }

    if (buf != NULL)
        memcpy(buf, xfer_buf, count * volume->sector_size);

    return DISK_SUCCESS;
}

static bool detect_sector_size(struct volume *volume) {
    struct dap dap = {0};

    if (xfer_buf == NULL) {
        xfer_buf = xfer_buf_alloc();
    }

    dap.size    = 16;
    dap.count   = 1;
    dap.segment = rm_seg(xfer_buf);
    dap.offset  = rm_off(xfer_buf);
    dap.lba     = 0;

    struct rm_regs r = {0};
    r.eax = 0x4200;
    r.edx = volume->drive;
    r.esi = (uint32_t)rm_off(&dap);
    r.ds  = rm_seg(&dap);

    struct rm_regs r_copy = r;
    struct dap dap_copy = dap;

    memset(xfer_buf, 0, XFER_BUF_SIZE);

    rm_int(0x13, &r, &r);

    if (r.eflags & EFLAGS_CF) {
        return false;
    }

    size_t sector_size_a = 0;
    for (long i = XFER_BUF_SIZE - 1; i >= 0; i--) {
        if (xfer_buf[i] != 0) {
            sector_size_a = i + 1;
            break;
        }
    }

    r = r_copy;
    dap = dap_copy;

    memset(xfer_buf, 0xff, XFER_BUF_SIZE);

    rm_int(0x13, &r, &r);

    if (r.eflags & EFLAGS_CF) {
        return false;
    }

    size_t sector_size_b = 0;
    for (long i = XFER_BUF_SIZE - 1; i >= 0; i--) {
        if (xfer_buf[i] != 0xff) {
            sector_size_b = i + 1;
            break;
        }
    }

    volume->sector_size = sector_size_a > sector_size_b ? sector_size_a : sector_size_b;

    if (volume->sector_size == 0) {
        return false;
    }

    return true;
}

void disk_create_index(uint8_t boot_drive) {
    // Disk count (only non-removable) at 0040:0075
    uint8_t bda_disk_count = mminb(rm_desegment(0x0040, 0x0075));

    int optical_indices = 1, hdd_indices = 1, consumed_bda_disks = 0;

    for (int drive = 0x80; drive <= 0xff; drive++) {
        // INT 13h has been seen to hang on drives from F0h up on some BIOSes,
        // while the boot drive is known to answer.
        if (drive >= 0xf0 && drive != boot_drive) {
            continue;
        }

        struct rm_regs r = {0};
        struct bios_drive_params drive_params = {0};

        r.eax = 0x4800;
        r.edx = drive;
        r.ds  = rm_seg(&drive_params);
        r.esi = rm_off(&drive_params);

        drive_params.buf_size = sizeof(struct bios_drive_params);

        rm_int(0x13, &r, &r);

        if (r.eflags & EFLAGS_CF) {
            continue;
        }

        bool is_removable = drive_params.info_flags & (1 << 2);

        struct dpte *dpte = NULL;
        if (drive_params.buf_size >= 0x1e
         && (drive_params.dpte_seg != 0x0000 || drive_params.dpte_off != 0x0000)
         && (drive_params.dpte_seg != 0xffff || drive_params.dpte_off != 0xffff)) {
            dpte = (void *)rm_desegment(drive_params.dpte_seg, drive_params.dpte_off);
            if ((dpte->control_port & 0xff00) != 0xa000) {
                // Check for removable (5) or ATAPI (6)
                is_removable = is_removable || ((dpte->flags & (1 << 5)) || (dpte->flags & (1 << 6)));
            }
        }

        struct volume *block = ext_mem_alloc(sizeof(struct volume));

        block->drive = drive;
        block->partition = 0;
        block->first_sect = 0;
        block->max_partition = -1;

        if (!detect_sector_size(block)) {
            pmm_free(block, sizeof(struct volume));
            continue;
        }

        // Normalize sect_count to 512-byte sectors for consistency with partitions
        // Preserve (uint64_t)-1 sentinel value (means "unknown size")
        if (drive_params.lba_count == (uint64_t)-1 || drive_params.lba_count == 0) {
            block->sect_count = (uint64_t)-1;
        } else {
            block->sect_count = drive_params.lba_count * (block->sector_size / 512);
        }

        // Detect optical drives via DPTE ATAPI bit (bit 6) or sector size heuristic
        bool is_atapi = (dpte != NULL && (dpte->flags & (1 << 6)));
        block->is_optical = is_atapi || (block->sector_size == 2048 && is_removable);

        // Ugly workaround for VMware, because it puts the optical drive at 0x9f but does
        // not expose DPTE.
        if (drive == 0x9f && block->sector_size == 2048) {
            is_removable = true;
            block->is_optical = true;
        }

        if (!is_removable && !block->is_optical) {
            // The count can omit the drive the BIOS just booted from, or be 0.
            if (consumed_bda_disks >= bda_disk_count && drive != boot_drive) {
                pmm_free(block, sizeof(struct volume));
                continue;
            }
            consumed_bda_disks++;
        }

        if (block->is_optical) {
            block->index = optical_indices++;
        } else {
            block->index = hdd_indices++;
        }

        block->fastest_xfer_size = fastest_xfer_size(block);

        if (gpt_get_guid(&block->guid, block)) {
            block->guid_valid = true;
        }

        // A filesystem occupying the whole medium, as ISO 9660 does, has no
        // partition volume to carry its label.
        char *fslabel = fs_get_label(block);
        if (fslabel != NULL) {
            block->fslabel_valid = true;
            block->fslabel = fslabel;
        }

        volume_index = pmm_realloc(
            volume_index,
            volume_index_i * sizeof(void *),
            (volume_index_i + 1) * sizeof(void *)
        );
        volume_index[volume_index_i++] = block;

        for (int part = 0; ; part++) {
            struct volume *p = ext_mem_alloc(sizeof(struct volume));
            int ret = part_get(p, block, part);

            if (ret == END_OF_TABLE || ret == INVALID_TABLE) {
                pmm_free(p, sizeof(struct volume));
                break;
            }
            if (ret == NO_PARTITION) {
                pmm_free(p, sizeof(struct volume));
                continue;
            }

            volume_index = pmm_realloc(
                volume_index,
                volume_index_i * sizeof(void *),
                (volume_index_i + 1) * sizeof(void *)
            );
            volume_index[volume_index_i++] = p;

            block->max_partition++;
        }
    }
}

#endif

#if defined(UEFI)

int disk_read_sectors(struct volume *volume, void *buf, uint64_t block, size_t count) {
    efi_status status;

    status = volume->block_io->read_blocks(volume->block_io,
                               volume->block_io->media->media_id,
                               block, count * volume->sector_size, buf);

    switch (status) {
        case EFI_SUCCESS: return DISK_SUCCESS;
        case EFI_NO_MEDIA: return DISK_NO_MEDIA;
        default: return DISK_FAILURE;
    }
}

static struct volume *pxe_from_efi_handle(efi_handle handle) {
    static struct volume *vol = NULL;

    efi_status status;

    struct efi_guid pxe_base_code_guid = EFI_PXE_BASE_CODE_PROTOCOL_GUID;
    struct efi_pxe_base_code_protocol *pxe_base_code = NULL;

    status = gBS->handle_protocol(handle, &pxe_base_code_guid, (void **)&pxe_base_code);
    if (status) {
        return NULL;
    }

    // There's only one PXE volume, and it belongs to the handle carrying the
    // protocol, so the lookup has to gate the reuse.
    if (vol) {
        return vol;
    }

    if (!pxe_base_code->mode->dhcp_discover_valid) {
        print("PXE somehow didn't use DHCP?\n");
        return NULL;
    }

    if (pxe_base_code->mode->using_ipv6) {
        print("Sorry, unsupported: PXE IPv6\n");
        return NULL;
    }

    vol = pxe_bind_volume(handle, pxe_base_code);
    return vol;
}

#define UNIQUE_SECTOR_POOL_SIZE 65536
static uint8_t *unique_sector_pool;
static size_t unique_sector_pool_alignment = 0;
static bool unique_sectors_calculated = false;

// ReadBlocks buffers must be aligned to IoAlign, which UEFI 2.11 13.9 limits
// to 0, 1, or a power of two: any other value is taken as 1, as for the volume
// caches. The pool is shared, so it moves when a device needs more alignment.
static void align_unique_sector_pool(struct efi_block_io_protocol *block_io) {
    size_t alignment = 4096;
    uint32_t io_align = block_io->media->io_align;
    if (io_align > alignment && (io_align & (io_align - 1)) == 0) {
        alignment = io_align;
    }

    if (alignment <= unique_sector_pool_alignment) {
        return;
    }

    // The allocation was rounded up to its alignment, both powers of two.
    pmm_free(unique_sector_pool, MAX((size_t)UNIQUE_SECTOR_POOL_SIZE, unique_sector_pool_alignment));
    unique_sector_pool = ext_mem_alloc_type_aligned(UNIQUE_SECTOR_POOL_SIZE,
        MEMMAP_BOOTLOADER_RECLAIMABLE, alignment);
    unique_sector_pool_alignment = alignment;
}

static void find_unique_sectors(void);

static struct volume *volume_by_unique_sector(void *b2b) {
    for (size_t i = 0; i < volume_index_i; i++) {
        if (volume_index[i]->unique_sector_valid == false) {
            continue;
        }

        if (memcmp(volume_index[i]->unique_sector_b2b, b2b, BLAKE2B_OUT_BYTES) == 0) {
            return volume_index[i];
        }
    }

    return NULL;
}

// Search for matching hash including invalidated volumes (for collision detection)
static struct volume *volume_by_sector_hash(void *b2b) {
    for (size_t i = 0; i < volume_index_i; i++) {
        if (volume_index[i]->unique_sector_valid == false
         && memcmp(volume_index[i]->unique_sector_b2b, (uint8_t[BLAKE2B_OUT_BYTES]){0}, BLAKE2B_OUT_BYTES) == 0) {
            // Hash was never set, skip
            continue;
        }

        if (memcmp(volume_index[i]->unique_sector_b2b, b2b, BLAKE2B_OUT_BYTES) == 0) {
            return volume_index[i];
        }
    }

    return NULL;
}

static bool is_efi_handle_to_skip(efi_handle handle) {
    efi_status status;

    struct efi_guid dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    struct efi_device_path_protocol *dp = NULL;

    struct efi_guid guids_to_skip[] = {
        // skip 7CCE9C94-983F-4D0A-8143-B6C05545B223 since it is apparently used by exposed
        // ROM devices that we do not want to touch
        // (see https://github.com/limine-bootloader/limine/issues/521#issuecomment-3160168795)
        {0x7CCE9C94, 0x983F, 0x4D0A, {0x81, 0x43, 0xB6, 0xC0, 0x55, 0x45, 0xB2, 0x23}},
    };

    status = gBS->handle_protocol(handle, &dp_guid, (void **)&dp);
    if (status) {
        return false;
    }

    for (;;) {
        if (dp->type == EFI_END_DEVICE_PATH_TYPE && dp->sub_type == EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE) {
            break;
        }

        uint16_t len = efi_device_path_node_length(dp);

        // Validate minimum device path node size before accessing type-specific data
        if (len < sizeof(struct efi_device_path_protocol)) {
            break;  // Malformed device path node
        }

        if (dp->type == EFI_HARDWARE_DEVICE_PATH && dp->sub_type == EFI_HW_VENDOR_DP) {
            // Vendor device path must be large enough to contain a GUID
            if (len >= sizeof(struct efi_device_path_protocol) + sizeof(struct efi_guid)) {
                void *vendor_guid = (void *)dp + sizeof(struct efi_device_path_protocol);

                for (size_t i = 0; i < SIZEOF_ARRAY(guids_to_skip); i++) {
                    if (memcmp(vendor_guid, &guids_to_skip[i], sizeof(struct efi_guid)) == 0) {
                        return true;
                    }
                }
            }
        }
        dp = (void *)dp + len;
    }

    return false;
}

static bool is_efi_handle_optical(efi_handle handle) {
    efi_status status;

    struct efi_guid dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    struct efi_device_path_protocol *dp = NULL;

    status = gBS->handle_protocol(handle, &dp_guid, (void **)&dp);
    if (status) {
        return false;
    }

    for (;;) {
        if (dp->type == EFI_END_DEVICE_PATH_TYPE && dp->sub_type == EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE) {
            break;
        }

        if (dp->type == EFI_MEDIA_DEVICE_PATH && dp->sub_type == EFI_MEDIA_CDROM_DP) {
            return true;
        }

        uint16_t len = efi_device_path_node_length(dp);
        if (len < sizeof(struct efi_device_path_protocol)) {
            break;  // Malformed device path node
        }
        dp = (void *)dp + len;
    }

    return false;
}

static struct efi_device_path_protocol *get_device_path(efi_handle handle) {
    efi_status status;
    struct efi_guid dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    struct efi_device_path_protocol *dp = NULL;

    status = gBS->handle_protocol(handle, &dp_guid, (void **)&dp);
    if (status) {
        return NULL;
    }
    return dp;
}

// Compare device paths up to (but not including) partition nodes
static bool device_paths_match_disk(struct efi_device_path_protocol *dp1,
                                    struct efi_device_path_protocol *dp2) {
    if (dp1 == NULL || dp2 == NULL) {
        return false;
    }

    while (!efi_is_device_path_end(dp1) && !efi_is_device_path_end(dp2)) {
        // Stop at partition nodes
        if (dp1->type == EFI_MEDIA_DEVICE_PATH &&
            (dp1->sub_type == EFI_MEDIA_HARDDRIVE_DP || dp1->sub_type == EFI_MEDIA_CDROM_DP)) {
            break;
        }
        if (dp2->type == EFI_MEDIA_DEVICE_PATH &&
            (dp2->sub_type == EFI_MEDIA_HARDDRIVE_DP || dp2->sub_type == EFI_MEDIA_CDROM_DP)) {
            break;
        }

        uint16_t len1 = efi_device_path_node_length(dp1);
        uint16_t len2 = efi_device_path_node_length(dp2);

        if (len1 != len2) {
            return false;
        }

        if (len1 < sizeof(struct efi_device_path_protocol)) {
            return false;
        }

        if (memcmp(dp1, dp2, len1) != 0) {
            return false;
        }

        dp1 = (void *)dp1 + len1;
        dp2 = (void *)dp2 + len2;
    }

    return true;
}

static struct volume *volume_by_device_path(efi_handle query_handle, bool cdrom_medium_fallback) {
    struct efi_device_path_protocol *query_dp = get_device_path(query_handle);
    if (query_dp == NULL) {
        return NULL;
    }

    // A CD-ROM node names an El Torito boot image (UEFI 2.11 10.3.5.2), which
    // has no volume unless a partition table describes it too, so the caller
    // may fall back to the medium holding it.
    struct volume *cdrom_medium = NULL;

    for (size_t i = 0; i < volume_index_i; i++) {
        struct efi_device_path_protocol *vol_dp = get_device_path(volume_index[i]->efi_handle);
        if (vol_dp == NULL) {
            continue;
        }

        if (device_paths_match_disk(query_dp, vol_dp)) {
            // Convert first_sect from 512-byte sectors to device LBAs
            int sector_size = volume_index[i]->sector_size;
            if ((volume_index[i]->first_sect * 512) % sector_size) {
                continue;  // Misaligned, skip this volume
            }
            uint64_t first_sect_lba = (volume_index[i]->first_sect * 512) / sector_size;

            struct efi_device_path_protocol *qp = query_dp;
            while (!efi_is_device_path_end(qp)) {
                if (qp->type == EFI_MEDIA_DEVICE_PATH && qp->sub_type == EFI_MEDIA_HARDDRIVE_DP) {
                    uint16_t len = efi_device_path_node_length(qp);
                    if (len < sizeof(struct efi_harddrive_device_path)) {
                        break;
                    }
                    struct efi_harddrive_device_path *query_hd = (void *)qp;
                    if (first_sect_lba == query_hd->partition_start) {
                        return volume_index[i];
                    }
                    break;
                }
                if (qp->type == EFI_MEDIA_DEVICE_PATH && qp->sub_type == EFI_MEDIA_CDROM_DP) {
                    uint16_t len = efi_device_path_node_length(qp);
                    if (len < sizeof(struct efi_cdrom_device_path)) {
                        break;
                    }
                    struct efi_cdrom_device_path *query_cd = (struct efi_cdrom_device_path *)qp;
                    if (first_sect_lba == query_cd->partition_start) {
                        return volume_index[i];
                    }
                    if (cdrom_medium == NULL && volume_index[i]->partition == 0) {
                        cdrom_medium = volume_index[i];
                    }
                    break;
                }
                uint16_t len = efi_device_path_node_length(qp);
                if (len < sizeof(struct efi_device_path_protocol)) {
                    break;
                }
                qp = (void *)qp + len;
            }

            if (efi_is_device_path_end(qp) && volume_index[i]->partition == 0) {
                return volume_index[i];
            }
        }
    }

    return cdrom_medium_fallback ? cdrom_medium : NULL;
}

static struct volume *volume_from_efi_handle(efi_handle handle, bool cdrom_medium_fallback) {
    efi_status status;

    struct efi_guid block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    struct efi_block_io_protocol *block_io = NULL;

    if (is_efi_handle_to_skip(handle)) {
        return NULL;
    }

    status = gBS->handle_protocol(handle, &block_io_guid, (void **)&block_io);
    if (status) {
        return pxe_from_efi_handle(handle);
    }

    // Try device path matching first (primary method)
    struct volume *ret = volume_by_device_path(handle, cdrom_medium_fallback);
    if (ret != NULL) {
        return ret;
    }

    // Fallback to unique sector matching
    uint64_t bdev_size = ((uint64_t)block_io->media->last_block + 1) * (uint64_t)block_io->media->block_size;
    if (bdev_size >= UNIQUE_SECTOR_POOL_SIZE) {
        // Pre-calculate unique sectors before reading query data into
        // the pool, since find_unique_sectors() uses the same buffer.
        find_unique_sectors();

        align_unique_sector_pool(block_io);

        status = block_io->read_blocks(block_io, block_io->media->media_id,
                                      0,
                                      UNIQUE_SECTOR_POOL_SIZE,
                                      unique_sector_pool);
        if (status == 0) {

            uint8_t b2b[BLAKE2B_OUT_BYTES];
            blake2b(b2b, unique_sector_pool, UNIQUE_SECTOR_POOL_SIZE);

            ret = volume_by_unique_sector(b2b);
            if (ret != NULL) {
                // Verify size, block size, and partition status match
                if (block_io->media->block_size == (uint32_t)ret->sector_size
                 && bdev_size == ret->sect_count * 512
                 && block_io->media->logical_partition == (ret->partition != 0)) {
                    return ret;
                }
            }
        }
    }

    // A Block I/O matching no volume does not rule out having booted over the
    // network from this handle: PXE stacks have been known to leave a
    // non-functional one behind on it.
    return pxe_from_efi_handle(handle);
}

struct volume *disk_volume_from_efi_handle(efi_handle handle) {
    return volume_from_efi_handle(handle, true);
}

static void find_unique_sectors(void) {
    if (unique_sectors_calculated) {
        return;
    }
    unique_sectors_calculated = true;

    efi_status status;

    for (size_t i = 0; i < volume_index_i; i++) {
        if ((volume_index[i]->first_sect * 512) % volume_index[i]->sector_size) {
            continue;
        }

        uint64_t first_sect = (volume_index[i]->first_sect * 512) / volume_index[i]->sector_size;

        // sect_count is always in 512-byte sectors
        if (volume_index[i]->sect_count * 512 < UNIQUE_SECTOR_POOL_SIZE) {
            continue;
        }

        align_unique_sector_pool(volume_index[i]->block_io);

        status = volume_index[i]->block_io->read_blocks(
                            volume_index[i]->block_io,
                            volume_index[i]->block_io->media->media_id,
                            first_sect,
                            UNIQUE_SECTOR_POOL_SIZE,
                            unique_sector_pool);
        if (status != 0) {
            continue;
        }

        uint8_t b2b[BLAKE2B_OUT_BYTES];
        blake2b(b2b, unique_sector_pool, UNIQUE_SECTOR_POOL_SIZE);

        // Check for collision BEFORE storing hash (so we don't find ourselves)
        // This searches all volumes including previously invalidated ones
        struct volume *collision = volume_by_sector_hash(b2b);

        // Always store the hash so future volumes can detect collisions
        memcpy(volume_index[i]->unique_sector_b2b, b2b, BLAKE2B_OUT_BYTES);

        if (collision == NULL) {
            volume_index[i]->unique_sector_valid = true;
            continue;
        }

        // Collision found - invalidate both volumes
        collision->unique_sector_valid = false;
        volume_index[i]->unique_sector_valid = false;
    }
}

static void find_part_handles(efi_handle *handles, size_t handle_count) {
    for (size_t i = 0; i < handle_count; i++) {
        // disk_create_index() clears the handles whose read test failed, and
        // the unique sector fallback would read 64K more from them.
        if (handles[i] == NULL) {
            continue;
        }

        // A disc keeps its own handle, not one of its El Torito images'.
        struct volume *vol = volume_from_efi_handle(handles[i], false);
        if (vol == NULL) {
            continue;
        }
        vol->efi_part_handle = handles[i];
    }
}

static bool is_efi_handle_driver_managed(efi_handle handle, struct efi_guid *protocol) {
    struct efi_open_protocol_information_entry *entries = NULL;
    size_t entries_count = 0;

    if (gBS->open_protocol_information(handle, protocol,
                                     &entries, &entries_count) != EFI_SUCCESS) {
        return false;
    }

    bool managed = false;
    for (size_t i = 0; i < entries_count; i++) {
        if (entries[i].attributes & (EFI_OPEN_PROTOCOL_BY_DRIVER
                                     | EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER)) {
            managed = true;
            break;
        }
    }

    if (entries != NULL) {
        gBS->free_pool(entries);
    }

    return managed;
}

static bool should_connect_storage_controller(efi_handle handle, bool bound) {
    struct efi_guid block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    void *block_io = NULL;
    if (gBS->handle_protocol(handle, &block_io_guid, &block_io) == EFI_SUCCESS) {
        // A disk or partition already claimed by a partition or filesystem
        // driver has nothing left to connect.
        struct efi_guid disk_io_guid = EFI_DISK_IO_PROTOCOL_GUID;
        return !is_efi_handle_driver_managed(handle, &disk_io_guid);
    }

    struct efi_guid pci_io_guid = EFI_PCI_IO_PROTOCOL_GUID;
    struct efi_pci_io_protocol *pci_io = NULL;
    if (gBS->handle_protocol(handle, &pci_io_guid, (void **)&pci_io) != EFI_SUCCESS) {
        return false;
    }

    // read prog-if, sub-class and base class; offsets 0x09..0x0B.
    uint8_t class_code[3] = {0};
    if (pci_io->pci.read(pci_io, EFI_PCI_IO_WIDTH_UINT8, 0x09, sizeof(class_code),
                         class_code) != EFI_SUCCESS) {
        return false;
    }

    uint8_t base_class = class_code[2];
    uint8_t sub_class = class_code[1];

    // 0x01: mass storage controller. 0x0C/0x03: USB host controller.
    if (base_class != 0x01 && !(base_class == 0x0C && sub_class == 0x03)) {
        return false;
    }

    // Unless asked to, only connect controllers no bus driver has bound yet
    // (the Fast Boot case of ticket #598). Re-connecting live controllers
    // makes some buggy firmware misbehave, up to spontaneous resets.
    return bound || !is_efi_handle_driver_managed(handle, &pci_io_guid);
}

// Recursively, so that their disks and partitions appear, without pulling in
// unrelated slow drivers.
static void connect_storage_controllers(bool bound) {
    efi_handle *all_handles = NULL;
    size_t all_handles_count = 0;
    if (gBS->locate_handle_buffer(EFI_ALL_HANDLES, NULL, NULL,
                                &all_handles_count, &all_handles) != EFI_SUCCESS) {
        return;
    }
    for (size_t i = 0; i < all_handles_count; i++) {
        if (should_connect_storage_controller(all_handles[i], bound)) {
            gBS->connect_controller(all_handles[i], NULL, NULL, true);
        }
    }
    gBS->free_pool(all_handles);
}

static bool is_efi_handle_indexed(efi_handle handle) {
    for (size_t i = 0; i < volume_index_i; i++) {
        if (volume_index[i]->efi_handle == handle) {
            return true;
        }
    }

    return false;
}

// Halve the transfer size until the device reads it. index_disks() has
// already read block 0 alone, so a failure here is down to the size. Reading
// through the cache spares the partition table a second read.
static void probe_xfer_size(struct volume *block) {
    uint8_t byte;
    while (block->fastest_xfer_size > 1 && !volume_read(block, &byte, 0, 1)) {
        block->fastest_xfer_size /= 2;
    }
}

// Kept across index_disks() calls, so that disks found later number after
// the ones found first.
static int optical_indices = 1, hdd_indices = 1;

// Index the disks, and their partitions, of the Block I/O handles not
// indexed yet. Returns whether there were any.
static bool index_disks(void) {
    efi_status status;

    efi_handle tmp_handles[1];

    struct efi_guid block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    efi_handle *handles = tmp_handles;
    size_t handles_size = sizeof(tmp_handles);

    status = gBS->locate_handle(EFI_BY_PROTOCOL, &block_io_guid, NULL, &handles_size, handles);

    // A network boot may have no disks at all.
    if (status == EFI_NOT_FOUND) {
        return false;
    }

    handles = ext_mem_alloc(handles_size);

    status = gBS->locate_handle(EFI_BY_PROTOCOL, &block_io_guid, NULL, &handles_size, handles);

    if (status != EFI_SUCCESS) {
        panic(false, "LocateHandle for BLOCK_IO_PROTOCOL failed. Machine not supported by Limine UEFI.");
    }

    size_t indexed_before = volume_index_i;

    size_t handle_count = handles_size / sizeof(efi_handle);

    for (size_t i = 0; i < handle_count; i++) {
        struct efi_block_io_protocol *drive = NULL;

        if (is_efi_handle_to_skip(handles[i]) || is_efi_handle_indexed(handles[i])) {
            continue;
        }

        status = gBS->handle_protocol(handles[i], &block_io_guid, (void **)&drive);

        if (status != 0 || drive == NULL || drive->media->last_block == 0)
            continue;

        if (drive->media->logical_partition)
            continue;

        if (drive->media->block_size == 0
         || drive->media->block_size > UNIQUE_SECTOR_POOL_SIZE) {
            continue;
        }

        align_unique_sector_pool(drive);

        // Read test to ensure device is responsive (skipping this causes hangs on some systems)
        status = drive->read_blocks(drive, drive->media->media_id, 0, drive->media->block_size, unique_sector_pool);
        if (status) {
            handles[i] = NULL;
            continue;
        }

        if (drive->media->last_block == UINT64_MAX) {
            continue;
        }

        struct volume *block = ext_mem_alloc(sizeof(struct volume));

        bool is_optical = is_efi_handle_optical(handles[i]) ||
                          (drive->media->read_only && drive->media->block_size == 2048);

        if (is_optical) {
            block->index = optical_indices++;
            block->is_optical = true;
        } else {
            block->index = hdd_indices++;
        }

        block->efi_handle = handles[i];
        block->block_io = drive;
        block->partition = 0;
        block->sector_size = drive->media->block_size;
        block->first_sect = 0;
        // Normalize sect_count to 512-byte sectors for consistency with partitions
        block->sect_count = (drive->media->last_block + 1) * (drive->media->block_size / 512);
        block->max_partition = -1;

        size_t granularity = 0;
        if (drive->revision >= EFI_BLOCK_IO_PROTOCOL_REVISION3) {
            granularity = drive->media->optimal_transfer_length_granularity;
        }

        // UEFI 2.11 13.9: the granularity is what transfers should be a
        // multiple of, not a transfer size.
        if (granularity == 0) {
            block->fastest_xfer_size = DEFAULT_FASTEST_XFER_SIZE;
        } else if (granularity > MAX_FASTEST_XFER_SIZE) {
            block->fastest_xfer_size = MAX_FASTEST_XFER_SIZE;
        } else {
            block->fastest_xfer_size = ALIGN_UP((size_t)DEFAULT_FASTEST_XFER_SIZE, granularity,
                panic(false, "disk: Transfer size overflow"));
            if (block->fastest_xfer_size > MAX_FASTEST_XFER_SIZE) {
                block->fastest_xfer_size = ALIGN_DOWN((size_t)MAX_FASTEST_XFER_SIZE, granularity);
            }
        }

        // Some firmware refuses a transfer of the size its own granularity
        // implies, with EFI_OUT_OF_RESOURCES, while smaller ones succeed.
        probe_xfer_size(block);

        if (gpt_get_guid(&block->guid, block)) {
            block->guid_valid = true;
        }

        // A filesystem occupying the whole medium, as ISO 9660 does, has no
        // partition volume to carry its label.
        char *fslabel = fs_get_label(block);
        if (fslabel != NULL) {
            block->fslabel_valid = true;
            block->fslabel = fslabel;
        }

        volume_index = pmm_realloc(
            volume_index,
            volume_index_i * sizeof(void *),
            (volume_index_i + 1) * sizeof(void *)
        );
        volume_index[volume_index_i++] = block;

        for (int part = 0; ; part++) {
            struct volume _p = {0};

            int ret = part_get(&_p, block, part);

            if (ret == END_OF_TABLE || ret == INVALID_TABLE)
                break;
            if (ret == NO_PARTITION)
                continue;

            struct volume *p = ext_mem_alloc(sizeof(struct volume));
            memcpy(p, &_p, sizeof(struct volume));

            volume_index = pmm_realloc(
                volume_index,
                volume_index_i * sizeof(void *),
                (volume_index_i + 1) * sizeof(void *)
            );
            volume_index[volume_index_i++] = p;

            block->max_partition++;
        }
    }

    find_part_handles(handles, handle_count);

    pmm_free(handles, handles_size);

    return volume_index_i != indexed_before;
}

void disk_create_index(void) {
    connect_storage_controllers(false);

    struct efi_guid block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    efi_handle handle;
    size_t handles_size = sizeof(handle);

    efi_status status = gBS->locate_handle(EFI_BY_PROTOCOL, &block_io_guid, NULL, &handles_size, &handle);

    // Only the first handle matters, so EFI_BUFFER_TOO_SMALL is not an error.
    if (status != EFI_BUFFER_TOO_SMALL && status != EFI_SUCCESS) {
        struct efi_guid pxe_guid = EFI_PXE_BASE_CODE_PROTOCOL_GUID;
        status = gBS->locate_handle(EFI_BY_PROTOCOL, &pxe_guid, NULL, &handles_size, &handle);
        // likewise, all that matters is that the protocol is present
        if (status == EFI_BUFFER_TOO_SMALL || status == EFI_SUCCESS) {
            return;
        }

        panic(false, "LocateHandle for BLOCK_IO_PROTOCOL failed. Machine not supported by Limine UEFI.");
    }

    index_disks();
}

bool disk_connect_all(void) {
    static bool connected = false;
    if (connected) {
        return false;
    }
    connected = true;

    connect_storage_controllers(true);

    if (!index_disks()) {
        return false;
    }

    // Volumes are told apart by content among all of them, the new ones too.
    if (unique_sectors_calculated) {
        for (size_t i = 0; i < volume_index_i; i++) {
            volume_index[i]->unique_sector_valid = false;
            memset(volume_index[i]->unique_sector_b2b, 0, BLAKE2B_OUT_BYTES);
        }
        unique_sectors_calculated = false;
    }

    return true;
}

#endif
