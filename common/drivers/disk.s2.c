#include <stdint.h>
#include <stddef.h>
#include <stdalign.h>
#include <drivers/disk.h>
#include <lib/libc.h>
#if defined (BIOS)
#  include <lib/real.h>
#elif defined (UEFI)
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

#if defined (BIOS)

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

static size_t fastest_xfer_size(struct volume *volume) {
    struct dap dap = {0};

    if (xfer_buf == NULL)
        xfer_buf = conv_mem_alloc(XFER_BUF_SIZE);

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

    if (xfer_buf == NULL)
        xfer_buf = conv_mem_alloc(XFER_BUF_SIZE);

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

static int disk_write_sectors(struct volume *volume, void *buf, uint64_t block, size_t count) {
    struct dap dap = {0};

    if (count * volume->sector_size > XFER_BUF_SIZE)
        panic(false, "XFER");

    if (xfer_buf == NULL)
        xfer_buf = conv_mem_alloc(XFER_BUF_SIZE);

    dap.size    = 16;
    dap.count   = count;
    dap.segment = rm_seg(xfer_buf);
    dap.offset  = rm_off(xfer_buf);
    dap.lba     = block;

    struct rm_regs r = {0};
    r.eax = 0x4301;
    r.edx = volume->drive;
    r.esi = (uint32_t)rm_off(&dap);
    r.ds  = rm_seg(&dap);

    if (buf != NULL)
        memcpy(xfer_buf, buf, count * volume->sector_size);

    rm_int(0x13, &r, &r);

    if (r.eflags & EFLAGS_CF) {
        return DISK_FAILURE;
    }

    return DISK_SUCCESS;
}

static bool detect_sector_size(struct volume *volume) {
    struct dap dap = {0};

    if (xfer_buf == NULL)
        xfer_buf = conv_mem_alloc(XFER_BUF_SIZE);

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

void disk_create_index(void) {
    // Disk count (only non-removable) at 0040:0075
    uint8_t bda_disk_count = mminb(rm_desegment(0x0040, 0x0075));

    int optical_indices = 1, hdd_indices = 1, consumed_bda_disks = 0;

    for (uint8_t drive = 0x80; drive < 0xf0; drive++) {
        struct rm_regs r = {0};
        struct bios_drive_params drive_params;

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
        block->sect_count = drive_params.lba_count;
        block->max_partition = -1;

        if (!detect_sector_size(block)) {
            continue;
        }

        if (disk_read_sectors(block, xfer_buf, 0, 1) != DISK_SUCCESS) {
            continue;
        }

        block->is_optical = (disk_write_sectors(block, xfer_buf, 0, 1) != DISK_SUCCESS) && block->sector_size == 2048;

        if (!is_removable && !block->is_optical) {
            if (consumed_bda_disks == bda_disk_count) {
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

        volume_index = pmm_realloc(
            volume_index,
            volume_index_i * sizeof(void *),
            (volume_index_i + 1) * sizeof(void *)
        );
        volume_index[volume_index_i++] = block;

        for (int part = 0; ; part++) {
            struct volume *p = ext_mem_alloc(sizeof(struct volume));
            int ret = part_get(p, block, part);

            if (ret == END_OF_TABLE || ret == INVALID_TABLE)
                break;
            if (ret == NO_PARTITION)
                continue;

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

#if defined (UEFI)

int disk_read_sectors(struct volume *volume, void *buf, uint64_t block, size_t count) {
    EFI_STATUS status;

    status = volume->block_io->ReadBlocks(volume->block_io,
                               volume->block_io->Media->MediaId,
                               block, count * volume->sector_size, buf);

    switch (status) {
        case EFI_SUCCESS: return DISK_SUCCESS;
        case EFI_NO_MEDIA: return DISK_NO_MEDIA;
        default: return DISK_FAILURE;
    }
}

static struct volume *pxe_from_efi_handle(EFI_HANDLE efi_handle) {
    static struct volume *vol = NULL;

    // There's only one PXE volume
    if (vol) {
        return vol;
    }

    EFI_STATUS status;

    EFI_GUID pxe_base_code_guid = EFI_PXE_BASE_CODE_PROTOCOL_GUID;
    EFI_PXE_BASE_CODE *pxe_base_code = NULL;

    status = gBS->HandleProtocol(efi_handle, &pxe_base_code_guid, (void **)&pxe_base_code);
    if (status) {
        return NULL;
    }

    if (!pxe_base_code->Mode->DhcpDiscoverValid) {
        print("PXE somehow didn't use DHCP?\n");
        return NULL;
    }

    if (pxe_base_code->Mode->UsingIpv6) {
        print("Sorry, unsupported: PXE IPv6\n");
        return NULL;
    }

    vol = pxe_bind_volume(efi_handle, pxe_base_code);
    return vol;
}

#define UNIQUE_SECTOR_POOL_SIZE 65536
static uint8_t *unique_sector_pool;

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

static bool validate_efi_handle(EFI_HANDLE efi_handle) {
    EFI_STATUS status;

    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *dp = NULL;

    status = gBS->HandleProtocol(efi_handle, &dp_guid, (void **)&dp);
    if (status) {
        return false;
    }

    for (;; dp = (void *)dp + *(uint16_t *)dp->Length) {
        if (dp->Type == END_DEVICE_PATH_TYPE && dp->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE) {
            break;
        }

        if (dp->Type != HARDWARE_DEVICE_PATH || dp->SubType != HW_PCI_DP) {
            continue;
        }

        return true;
    }

    return false;
}

struct volume *disk_volume_from_efi_handle(EFI_HANDLE efi_handle) {
    struct volume *ret;

    EFI_STATUS status;

    EFI_GUID block_io_guid = BLOCK_IO_PROTOCOL;
    EFI_BLOCK_IO *block_io = NULL;

    if (!validate_efi_handle(efi_handle)) {
        return NULL;
    }

    status = gBS->HandleProtocol(efi_handle, &block_io_guid, (void **)&block_io);
    if (status) {
        return pxe_from_efi_handle(efi_handle);
    }

    uint64_t bdev_size = ((uint64_t)block_io->Media->LastBlock + 1) * (uint64_t)block_io->Media->BlockSize;
    if (bdev_size < UNIQUE_SECTOR_POOL_SIZE) {
        goto fallback;
    }

    status = block_io->ReadBlocks(block_io, block_io->Media->MediaId,
                                  0,
                                  UNIQUE_SECTOR_POOL_SIZE,
                                  unique_sector_pool);
    if (status != 0) {
        goto fallback;
    }

    uint8_t b2b[BLAKE2B_OUT_BYTES];
    blake2b(b2b, unique_sector_pool, UNIQUE_SECTOR_POOL_SIZE);

    ret = volume_by_unique_sector(b2b);
    if (ret != NULL) {
        return ret;
    }

    // Fallback to read-back method
fallback:;
    uint64_t signature = rand64();
    uint64_t new_signature;
    do { new_signature = rand64(); } while (new_signature == signature);
    uint64_t orig;

    status = block_io->ReadBlocks(block_io, block_io->Media->MediaId, 0, 4096, unique_sector_pool);
    orig = *(uint64_t *)unique_sector_pool;
    if (status) {
        return NULL;
    }

    *(uint64_t *)unique_sector_pool = signature;
    status = block_io->WriteBlocks(block_io, block_io->Media->MediaId, 0, 4096, unique_sector_pool);
    if (status) {
        // Attempt to restore anyways, just in case.
        *(uint64_t *)unique_sector_pool = orig;
        block_io->WriteBlocks(block_io, block_io->Media->MediaId, 0, 4096, unique_sector_pool);
        return NULL;
    }

    ret = NULL;
    for (size_t i = 0; i < volume_index_i; i++) {
        uint64_t compare;

        status = volume_index[i]->block_io->ReadBlocks(volume_index[i]->block_io,
                          volume_index[i]->block_io->Media->MediaId,
                          (volume_index[i]->first_sect * 512) / volume_index[i]->sector_size,
                          4096, unique_sector_pool);
        compare = *(uint64_t *)unique_sector_pool;
        if (status) {
            continue;
        }

        if (compare == signature) {
            // Double check
            status = block_io->ReadBlocks(block_io, block_io->Media->MediaId, 0, 4096, unique_sector_pool);
            if (status) {
                break;
            }
            *(uint64_t *)unique_sector_pool = new_signature;
            status = block_io->WriteBlocks(block_io, block_io->Media->MediaId, 0, 4096, unique_sector_pool);
            if (status) {
                break;
            }

            status = volume_index[i]->block_io->ReadBlocks(volume_index[i]->block_io,
                          volume_index[i]->block_io->Media->MediaId,
                          (volume_index[i]->first_sect * 512) / volume_index[i]->sector_size,
                          4096, unique_sector_pool);
            compare = *(uint64_t *)unique_sector_pool;
            if (status) {
                continue;
            }

            if (compare == new_signature) {
                ret = volume_index[i];
                break;
            }

            status = block_io->ReadBlocks(block_io, block_io->Media->MediaId, 0, 4096, unique_sector_pool);
            if (status) {
                break;
            }
            *(uint64_t *)unique_sector_pool = signature;
            status = block_io->WriteBlocks(block_io, block_io->Media->MediaId, 0, 4096, unique_sector_pool);
            if (status) {
                break;
            }
        }
    }

    status = block_io->ReadBlocks(block_io, block_io->Media->MediaId, 0, 4096, unique_sector_pool);
    if (status) {
        return NULL;
    }
    *(uint64_t *)unique_sector_pool = orig;
    status = block_io->WriteBlocks(block_io, block_io->Media->MediaId, 0, 4096, unique_sector_pool);
    if (status) {
        return NULL;
    }

    if (ret != NULL) {
        return ret;
    }

    return NULL;
}

static void find_unique_sectors(void) {
    EFI_STATUS status;

    for (size_t i = 0; i < volume_index_i; i++) {
        if ((volume_index[i]->first_sect * 512) % volume_index[i]->sector_size) {
            continue;
        }

        size_t first_sect = (volume_index[i]->first_sect * 512) / volume_index[i]->sector_size;

        if (volume_index[i]->sect_count * volume_index[i]->sector_size < UNIQUE_SECTOR_POOL_SIZE) {
            continue;
        }

        status = volume_index[i]->block_io->ReadBlocks(
                            volume_index[i]->block_io,
                            volume_index[i]->block_io->Media->MediaId,
                            first_sect,
                            UNIQUE_SECTOR_POOL_SIZE,
                            unique_sector_pool);
        if (status != 0) {
            continue;
        }

        uint8_t b2b[BLAKE2B_OUT_BYTES];
        blake2b(b2b, unique_sector_pool, UNIQUE_SECTOR_POOL_SIZE);

        struct volume *collision = volume_by_unique_sector(b2b);
        if (collision == NULL) {
            volume_index[i]->unique_sector_valid = true;
            memcpy(volume_index[i]->unique_sector_b2b, b2b, BLAKE2B_OUT_BYTES);
            continue;
        }

        // Invalidate collision's unique sector
        collision->unique_sector_valid = false;
    }
}

static void find_part_handles(EFI_HANDLE *handles, size_t handle_count) {
    for (size_t i = 0; i < handle_count; i++) {
        struct volume *vol = disk_volume_from_efi_handle(handles[i]);
        if (vol == NULL) {
            continue;
        }
        vol->efi_part_handle = handles[i];
    }
}

void disk_create_index(void) {
    EFI_STATUS status;

    unique_sector_pool = ext_mem_alloc(UNIQUE_SECTOR_POOL_SIZE);

    EFI_HANDLE tmp_handles[1];

    EFI_GUID block_io_guid = BLOCK_IO_PROTOCOL;
    EFI_HANDLE *handles = tmp_handles;
    UINTN handles_size = sizeof(tmp_handles);

    status = gBS->LocateHandle(ByProtocol, &block_io_guid, NULL, &handles_size, handles);

    // we only care about the first handle, so ignore if we get EFI_BUFFER_TOO_SMALL
    if (status != EFI_BUFFER_TOO_SMALL && status != EFI_SUCCESS) {
        EFI_GUID pxe_guid = EFI_PXE_BASE_CODE_PROTOCOL_GUID;
        status = gBS->LocateHandle(ByProtocol, &pxe_guid, NULL, &handles_size, handles);
        // likewise, all that matters is that the protocol is present
        if (status == EFI_BUFFER_TOO_SMALL || status == EFI_SUCCESS) {
            return;
        }

        goto fail;
    }

    handles = ext_mem_alloc(handles_size);

    status = gBS->LocateHandle(ByProtocol, &block_io_guid, NULL, &handles_size, handles);

    if (status != EFI_SUCCESS) {
fail:
        panic(false, "LocateHandle for BLOCK_IO_PROTOCOL failed. Machine not supported by Limine UEFI.");
    }

    int optical_indices = 1, hdd_indices = 1;

    size_t handle_count = handles_size / sizeof(EFI_HANDLE);

    for (size_t i = 0; i < handle_count; i++) {
        EFI_BLOCK_IO *drive = NULL;

        if (!validate_efi_handle(handles[i])) {
            continue;
        }

        status = gBS->HandleProtocol(handles[i], &block_io_guid, (void **)&drive);

        if (status != 0 || drive == NULL || drive->Media->LastBlock == 0)
            continue;

        if (drive->Media->LogicalPartition)
            continue;

        drive->Media->WriteCaching = false;

        status = drive->ReadBlocks(drive, drive->Media->MediaId, 0, 4096, unique_sector_pool);
        if (status) {
            continue;
        }

        status = drive->WriteBlocks(drive, drive->Media->MediaId, 0, 4096, unique_sector_pool);

        drive->Media->WriteCaching = true;

        struct volume *block = ext_mem_alloc(sizeof(struct volume));

        if ((status || drive->Media->ReadOnly) && drive->Media->BlockSize == 2048) {
            block->index = optical_indices++;
            block->is_optical = true;
        } else {
            block->index = hdd_indices++;
        }

        block->efi_handle = handles[i];
        block->block_io = drive;
        block->partition = 0;
        block->sector_size = drive->Media->BlockSize;
        block->first_sect = 0;
        block->sect_count = drive->Media->LastBlock + 1;
        block->max_partition = -1;

        if (drive->Revision >= EFI_BLOCK_IO_PROTOCOL_REVISION3) {
            block->fastest_xfer_size = drive->Media->OptimalTransferLengthGranularity;
        }

        if (block->fastest_xfer_size == 0) {
            block->fastest_xfer_size = DEFAULT_FASTEST_XFER_SIZE;
        } else if (block->fastest_xfer_size >= MAX_FASTEST_XFER_SIZE) {
            block->fastest_xfer_size = MAX_FASTEST_XFER_SIZE;
        }

        if (gpt_get_guid(&block->guid, block)) {
            block->guid_valid = true;
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

    find_unique_sectors();
    find_part_handles(handles, handle_count);

    pmm_free(handles, handles_size);
}

#endif
