#if defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <efi.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/trueos_hii.h>
#include <mm/pmm.h>

#define TRPAY1_VERSION 1

#define SEC_STATUS        1
#define SEC_HII           2
#define SEC_CONFIG        3
#define SEC_BOOT_SERVICES 4

#define CAPTURE_FLAG_HII_DATABASE           (1u << 0)
#define CAPTURE_FLAG_HII_PACKAGES           (1u << 1)
#define CAPTURE_FLAG_CONFIG_ROUTING         (1u << 3)
#define CAPTURE_FLAG_CONFIG                 (1u << 4)
#define CAPTURE_FLAG_BOOT_SERVICES_RETAINED (1u << 31)

#define MAX_HII_PACKAGE_BYTES (12u * 1024u * 1024u)
#define MAX_HII_CONFIG_BYTES  (4u * 1024u * 1024u)
#define MAX_PAYLOAD_BYTES     (16u * 1024u * 1024u)
#define MAX_RETAINED_BOOT_SERVICES_RANGES 512u

#define BOOT_SERVICES_RANGE_EXECUTABLE (1u << 0)
#define BOOT_SERVICES_RANGE_ENTRYPOINT  (1u << 1)
#define BOOT_SERVICES_RANGE_TABLE       (1u << 2)

#define BOOT_SERVICES_SET_COMPLETE          (1u << 0)
#define BOOT_SERVICES_SET_WATCHDOG_DISABLED (1u << 1)
#define BOOT_SERVICES_SET_EXIT_GROUP_SENT   (1u << 2)

#define TRUEOS_HII_DATABASE_PROTOCOL_GUID \
    { 0xef9fc172, 0xa1b2, 0x4693, { 0xb3, 0x27, 0x6d, 0x32, 0xfc, 0x41, 0x60, 0x42 } }
#define TRUEOS_HII_CONFIG_ROUTING_PROTOCOL_GUID \
    { 0x587e72d7, 0xcc50, 0x4f79, { 0x82, 0x09, 0xca, 0x29, 0x1f, 0xc1, 0xa1, 0x0f } }
#define TRUEOS_EXIT_BOOT_SERVICES_EVENT_GROUP_GUID \
    { 0x27abf055, 0xb1b8, 0x4c26, { 0x80, 0x48, 0x74, 0x8f, 0x37, 0xba, 0xa2, 0xdf } }

// Only the members this file actually calls; real firmware structs are
// longer, but C field access only needs an accurate layout up to the member
// used, matching the equivalent trimmed-down bindings in bios_capture.rs.
typedef struct {
    void *NewPackageList;
    void *RemovePackageList;
    void *UpdatePackageList;
    void *ListPackageLists;
    EFI_STATUS (EFIAPI *ExportPackageLists)(void *This, void *Handle, UINTN *BufferSize, void *Buffer);
} TRUEOS_HII_DATABASE_PROTOCOL;

typedef struct {
    void *ExtractConfig;
    EFI_STATUS (EFIAPI *ExportConfig)(void *This, uint16_t **Results);
} TRUEOS_HII_CONFIG_ROUTING_PROTOCOL;

#pragma pack(push, 1)
struct trpay1_header {
    uint8_t  magic[8];
    uint16_t version;
    uint16_t header_bytes;
    uint16_t section_entry_bytes;
    uint16_t reserved0;
    uint32_t section_count;
    uint32_t total_bytes;
    uint32_t capture_flags;
    uint32_t reserved1;
};

struct trpay1_section {
    uint32_t kind;
    uint32_t flags;
    uint32_t offset;
    uint32_t length;
    uint32_t crc32;
    uint32_t reserved;
    uint64_t status;
};

struct trstat1_status {
    uint8_t  magic[8];
    uint16_t version;
    uint16_t bytes;
    uint32_t flags;
    uint64_t hii_database_locate_status;
    uint64_t hii_export_query_status;
    uint64_t hii_export_status;
    uint64_t hii_parse_status;
    uint32_t hii_bytes;
    uint32_t package_lists;
    uint32_t form_packages;
    uint32_t string_packages;
    uint64_t config_routing_locate_status;
    uint64_t config_export_status;
    uint32_t config_bytes;
    uint32_t reserved;
};

struct trbsr1_header {
    uint8_t  magic[8];
    uint16_t version;
    uint16_t header_bytes;
    uint16_t entry_bytes;
    uint16_t reserved0;
    uint32_t count;
    uint32_t flags;
};

struct trbsr1_entry {
    uint64_t physical_start;
    uint64_t length;
    uint32_t memory_type;
    uint32_t flags;
};
#pragma pack(pop)

typedef EFI_STATUS (EFIAPI *TRUEOS_EXIT_BOOT_SERVICES)(EFI_HANDLE ImageHandle, UINTN MapKey);

static TRUEOS_EXIT_BOOT_SERVICES trueos_original_exit_boot_services = NULL;
static uint32_t *trueos_capture_flags = NULL;
static struct trbsr1_header *trueos_retained_ranges = NULL;
static struct trpay1_section *trueos_retained_section = NULL;
static bool trueos_quiesce_completed = false;
static bool trueos_watchdog_disabled = false;
static bool trueos_exit_group_signalled = false;

static bool trueos_refresh_boot_services_crc(void) {
    if (gBS == NULL || gBS->CalculateCrc32 == NULL || gBS->Hdr.HeaderSize < sizeof(gBS->Hdr)) {
        return false;
    }

    UINT32 crc = 0;
    gBS->Hdr.CRC32 = 0;
    EFI_STATUS status = gBS->CalculateCrc32(gBS, gBS->Hdr.HeaderSize, &crc);
    if (EFI_ERROR(status)) {
        return false;
    }
    gBS->Hdr.CRC32 = crc;
    return true;
}

static bool trueos_descriptor_bounds(EFI_MEMORY_DESCRIPTOR *entry, uint64_t *base, uint64_t *top) {
    if (entry == NULL || entry->NumberOfPages == 0 || entry->NumberOfPages > UINT64_MAX / 4096) {
        return false;
    }
    uint64_t bytes = (uint64_t)entry->NumberOfPages * 4096;
    if (entry->PhysicalStart > UINT64_MAX - bytes) {
        return false;
    }
    *base = entry->PhysicalStart;
    *top = entry->PhysicalStart + bytes;
    return true;
}

static bool trueos_descriptor_contains(EFI_MEMORY_DESCRIPTOR *entry, uintptr_t address) {
    uint64_t base, top;
    if (!trueos_descriptor_bounds(entry, &base, &top)) {
        return false;
    }
    return (uint64_t)address >= base && (uint64_t)address < top;
}

static bool trueos_descriptor_contains_boot_service_entrypoint(EFI_MEMORY_DESCRIPTOR *entry) {
    if (gBS == NULL || gBS->Hdr.HeaderSize < sizeof(gBS->Hdr)) {
        return false;
    }

    size_t header_size = gBS->Hdr.HeaderSize;
    if (header_size > sizeof(*gBS)) {
        header_size = sizeof(*gBS);
    }

    const uint8_t *bytes = (const uint8_t *)gBS;
    for (size_t offset = sizeof(gBS->Hdr);
         offset + sizeof(uintptr_t) <= header_size;
         offset += sizeof(uintptr_t)) {
        uintptr_t target = 0;
        memcpy(&target, bytes + offset, sizeof(target));
        if (target != 0 && trueos_descriptor_contains(entry, target)) {
            return true;
        }
    }
    return false;
}

static bool trueos_signal_exit_boot_services_group(void) {
    if (gBS == NULL || gBS->CreateEventEx == NULL || gBS->SignalEvent == NULL || gBS->CloseEvent == NULL) {
        return false;
    }

    EFI_GUID group = TRUEOS_EXIT_BOOT_SERVICES_EVENT_GROUP_GUID;
    EFI_EVENT event = NULL;
    EFI_STATUS create = gBS->CreateEventEx(0, 0, NULL, NULL, &group, &event);
    if (EFI_ERROR(create) || event == NULL) {
        return false;
    }

    EFI_STATUS signal = gBS->SignalEvent(event);
    EFI_STATUS close = gBS->CloseEvent(event);
    return !EFI_ERROR(signal) && !EFI_ERROR(close);
}

static bool trueos_prepare_firmware_quiesce(void) {
    if (trueos_quiesce_completed) {
        return true;
    }

    trueos_watchdog_disabled = false;
    if (gBS != NULL && gBS->SetWatchdogTimer != NULL) {
        EFI_STATUS watchdog = gBS->SetWatchdogTimer(0, 0, 0, NULL);
        trueos_watchdog_disabled = !EFI_ERROR(watchdog);
    }

    trueos_exit_group_signalled = trueos_signal_exit_boot_services_group();
    if (!trueos_exit_group_signalled) {
        return false;
    }

    trueos_quiesce_completed = true;
    return true;
}

static bool trueos_protect_boot_services_in_limine_map(void) {
    if (efi_mmap == NULL || efi_desc_size < sizeof(EFI_MEMORY_DESCRIPTOR) || efi_desc_size == 0
     || trueos_retained_ranges == NULL || trueos_retained_section == NULL
     || gBS == NULL || gBS->CalculateCrc32 == NULL) {
        return false;
    }

    UINTN count = efi_mmap_size / efi_desc_size;
    UINTN retained_count = 0;
    bool table_found = false;
    bool crc_entrypoint_found = false;
    uintptr_t crc_target = (uintptr_t)gBS->CalculateCrc32;

    // Retain the normal BootServicesCode/Data set plus any descriptor that
    // contains the Boot Services table or a direct function entrypoint. Some
    // real firmware places DXE core entrypoints in a non-BootServicesCode EFI
    // memory type; the first bare-metal TRUEOS probe observed exactly that for
    // CalculateCrc32().
    for (UINTN i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR *entry = (void *)((uint8_t *)efi_mmap + i * efi_desc_size);
        if (entry->NumberOfPages == 0) {
            continue;
        }

        bool table = trueos_descriptor_contains(entry, (uintptr_t)gBS);
        bool entrypoint = trueos_descriptor_contains_boot_service_entrypoint(entry);
        bool standard = entry->Type == EfiBootServicesCode || entry->Type == EfiBootServicesData;
        if (!standard && !table && !entrypoint) {
            continue;
        }

        if (entry->NumberOfPages > UINT64_MAX / 4096
         || retained_count == MAX_RETAINED_BOOT_SERVICES_RANGES) {
            return false;
        }
        retained_count++;
        table_found |= table;
        crc_entrypoint_found |= trueos_descriptor_contains(entry, crc_target);
    }
    if (retained_count == 0 || !table_found || !crc_entrypoint_found) {
        return false;
    }

    struct trbsr1_entry *ranges = (void *)((uint8_t *)trueos_retained_ranges
        + sizeof(struct trbsr1_header));
    memset(ranges, 0,
           MAX_RETAINED_BOOT_SERVICES_RANGES * sizeof(struct trbsr1_entry));

    UINTN out = 0;
    for (UINTN i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR *entry = (void *)((uint8_t *)efi_mmap + i * efi_desc_size);
        if (entry->NumberOfPages == 0) {
            continue;
        }

        bool table = trueos_descriptor_contains(entry, (uintptr_t)gBS);
        bool entrypoint = trueos_descriptor_contains_boot_service_entrypoint(entry);
        bool standard = entry->Type == EfiBootServicesCode || entry->Type == EfiBootServicesData;
        if (!standard && !table && !entrypoint) {
            continue;
        }

        ranges[out].physical_start = entry->PhysicalStart;
        ranges[out].length = (uint64_t)entry->NumberOfPages * 4096;
        ranges[out].memory_type = entry->Type;
        ranges[out].flags = 0;
        if (entry->Type == EfiBootServicesCode || entrypoint) {
            ranges[out].flags |= BOOT_SERVICES_RANGE_EXECUTABLE;
        }
        if (entrypoint) {
            ranges[out].flags |= BOOT_SERVICES_RANGE_ENTRYPOINT;
        }
        if (table) {
            ranges[out].flags |= BOOT_SERVICES_RANGE_TABLE;
        }
        out++;

        // This edits Limine's final memory-map copy only. Firmware still owns
        // these descriptors because ExitBootServices is not actually called;
        // TRUEOS must therefore never reclaim them.
        entry->Type = EfiReservedMemoryType;
    }

    trueos_retained_ranges->count = (uint32_t)out;
    trueos_retained_ranges->flags = BOOT_SERVICES_SET_COMPLETE;
    if (trueos_watchdog_disabled) {
        trueos_retained_ranges->flags |= BOOT_SERVICES_SET_WATCHDOG_DISABLED;
    }
    if (trueos_exit_group_signalled) {
        trueos_retained_ranges->flags |= BOOT_SERVICES_SET_EXIT_GROUP_SENT;
    }
    trueos_retained_section->flags = 1; // captured
    trueos_retained_section->status = (uint64_t)EFI_SUCCESS;

    UINT32 crc = 0;
    EFI_STATUS crc_status = gBS->CalculateCrc32(
        trueos_retained_ranges,
        trueos_retained_section->length,
        &crc
    );
    if (EFI_ERROR(crc_status)) {
        return false;
    }
    trueos_retained_section->crc32 = crc;
    return true;
}

static EFI_STATUS EFIAPI trueos_retain_exit_boot_services(EFI_HANDLE image_handle, UINTN map_key) {
    TRUEOS_EXIT_BOOT_SERVICES original = trueos_original_exit_boot_services;

    // First pass: perform the firmware driver's normal ExitBootServices event
    // notification without actually ending Boot Services. Signalling an event
    // group is explicitly supported by UEFI; returning an error then makes
    // Limine reacquire a fresh memory map after any callbacks have quiesced
    // device-facing DXE state.
    if (!trueos_quiesce_completed) {
        if (!trueos_prepare_firmware_quiesce()) {
            if (original != NULL) {
                gBS->ExitBootServices = original;
                (void)trueos_refresh_boot_services_crc();
                return original(image_handle, map_key);
            }
            return EFI_ABORTED;
        }
        return EFI_INVALID_PARAMETER;
    }

    bool protected = trueos_protect_boot_services_in_limine_map();

    // Leave the table in its firmware-provided shape for TRUEOS. The hook is
    // only for Limine's handoff calls; TRUEOS gets the original entry back.
    if (original != NULL) {
        gBS->ExitBootServices = original;
    }
    bool table_crc_ok = trueos_refresh_boot_services_crc();

    // Retention is opt-in and self-proving. If the final map could not be made
    // non-reclaimable, the capture payload is unavailable, or the table could
    // not be restored consistently, fall back to the real firmware handoff.
    if (!protected || !table_crc_ok || trueos_capture_flags == NULL || original == NULL) {
        if (original != NULL) {
            return original(image_handle, map_key);
        }
        return EFI_ABORTED;
    }

    // The kernel only trusts this bit. It is written after Limine actually
    // attempted its handoff and we deliberately intercepted it, so an older
    // patched Limine cannot accidentally make TRUEOS call dead Boot Services.
    *trueos_capture_flags |= CAPTURE_FLAG_BOOT_SERVICES_RETAINED;
    return EFI_SUCCESS;
}

static bool trueos_arm_boot_services_retention(void) {
    if (trueos_original_exit_boot_services != NULL) {
        return true;
    }
    if (gBS == NULL || gBS->ExitBootServices == NULL) {
        return false;
    }

    trueos_original_exit_boot_services = gBS->ExitBootServices;
    gBS->ExitBootServices = trueos_retain_exit_boot_services;
    if (trueos_refresh_boot_services_crc()) {
        return true;
    }

    gBS->ExitBootServices = trueos_original_exit_boot_services;
    (void)trueos_refresh_boot_services_crc();
    trueos_original_exit_boot_services = NULL;
    return false;
}

static uint64_t align_up_u64(uint64_t value, uint64_t alignment) {
    return (value + (alignment - 1)) & ~(alignment - 1);
}

static uint32_t crc32_of(const void *data, UINTN len) {
    uint32_t crc = 0;
    if (len == 0) {
        return 0;
    }
    gBS->CalculateCrc32((void *)data, len, &crc);
    return crc;
}

bool trueos_hii_capture(void **out_address, size_t *out_size) {
    trueos_capture_flags = NULL;
    trueos_retained_ranges = NULL;
    trueos_retained_section = NULL;
    trueos_quiesce_completed = false;
    trueos_watchdog_disabled = false;
    trueos_exit_group_signalled = false;

    struct trstat1_status status;
    memset(&status, 0, sizeof(status));
    memcpy(status.magic, "TRSTAT1\0", 8);
    status.version = TRPAY1_VERSION;
    status.bytes = sizeof(status);
    status.hii_database_locate_status = (uint64_t)EFI_NOT_FOUND;
    status.hii_export_query_status = (uint64_t)EFI_NOT_STARTED;
    status.hii_export_status = (uint64_t)EFI_NOT_STARTED;
    status.hii_parse_status = (uint64_t)EFI_NOT_STARTED;
    status.config_routing_locate_status = (uint64_t)EFI_NOT_FOUND;
    status.config_export_status = (uint64_t)EFI_NOT_STARTED;

    // HII package lists.
    void *hii_buffer = NULL;
    UINTN hii_len = 0;
    {
        EFI_GUID guid = TRUEOS_HII_DATABASE_PROTOCOL_GUID;
        void *interface = NULL;
        EFI_STATUS locate = gBS->LocateProtocol(&guid, NULL, &interface);
        status.hii_database_locate_status = (uint64_t)locate;

        if (!EFI_ERROR(locate) && interface != NULL) {
            status.flags |= CAPTURE_FLAG_HII_DATABASE;
            TRUEOS_HII_DATABASE_PROTOCOL *hii_db = (TRUEOS_HII_DATABASE_PROTOCOL *)interface;

            UINTN bytes = 0;
            EFI_STATUS query = hii_db->ExportPackageLists(interface, NULL, &bytes, NULL);
            status.hii_export_query_status = (uint64_t)query;

            bool query_ok = !EFI_ERROR(query)
                || query == EFI_BUFFER_TOO_SMALL
                || query == EFI_OUT_OF_RESOURCES;

            if (query_ok && bytes != 0 && bytes <= MAX_HII_PACKAGE_BYTES) {
                void *buffer = NULL;
                EFI_STATUS alloc = gBS->AllocatePool(EfiLoaderData, bytes, &buffer);
                if (!EFI_ERROR(alloc) && buffer != NULL) {
                    UINTN exported_bytes = bytes;
                    EFI_STATUS export = hii_db->ExportPackageLists(interface, NULL, &exported_bytes, buffer);
                    status.hii_export_status = (uint64_t)export;
                    if (!EFI_ERROR(export) && exported_bytes != 0 && exported_bytes <= bytes) {
                        hii_buffer = buffer;
                        hii_len = exported_bytes;
                        status.hii_bytes = (uint32_t)exported_bytes;
                        status.flags |= CAPTURE_FLAG_HII_PACKAGES;
                    } else {
                        gBS->FreePool(buffer);
                    }
                } else {
                    status.hii_export_status = (uint64_t)alloc;
                }
            }
        }
    }

    // Current HII configuration (redacted content; bounded metadata only
    // downstream). NUL-terminated UTF-16, per EFI_HII_CONFIG_ROUTING_PROTOCOL.
    uint16_t *config_buffer = NULL;
    UINTN config_len = 0;
    {
        EFI_GUID guid = TRUEOS_HII_CONFIG_ROUTING_PROTOCOL_GUID;
        void *interface = NULL;
        EFI_STATUS locate = gBS->LocateProtocol(&guid, NULL, &interface);
        status.config_routing_locate_status = (uint64_t)locate;

        if (!EFI_ERROR(locate) && interface != NULL) {
            status.flags |= CAPTURE_FLAG_CONFIG_ROUTING;
            TRUEOS_HII_CONFIG_ROUTING_PROTOCOL *routing = (TRUEOS_HII_CONFIG_ROUTING_PROTOCOL *)interface;

            uint16_t *config = NULL;
            EFI_STATUS export = routing->ExportConfig(interface, &config);
            status.config_export_status = (uint64_t)export;

            if (!EFI_ERROR(export) && config != NULL) {
                UINTN max_units = MAX_HII_CONFIG_BYTES / sizeof(uint16_t);
                UINTN units = 0;
                bool terminated = false;
                while (units < max_units) {
                    if (config[units] == 0) {
                        units++;
                        terminated = true;
                        break;
                    }
                    units++;
                }
                if (terminated) {
                    config_buffer = config;
                    config_len = units * sizeof(uint16_t);
                    status.config_bytes = (uint32_t)config_len;
                    status.flags |= CAPTURE_FLAG_CONFIG;
                } else {
                    status.config_export_status = (uint64_t)EFI_BAD_BUFFER_SIZE;
                    gBS->FreePool(config);
                }
            }
        }
    }

    // Assemble the TRPAY1 payload: header, section directory, then sections.
    // SEC_BOOT_SERVICES is always reserved in this experimental payload. Its
    // fixed-capacity body is filled by the final ExitBootServices interception,
    // when Limine has the actual final EFI memory map in hand.
    size_t section_count = 2 + (hii_len != 0 ? 1 : 0) + (config_len != 0 ? 1 : 0);
    uint64_t cursor = sizeof(struct trpay1_header)
                     + (uint64_t)section_count * sizeof(struct trpay1_section);
    cursor = align_up_u64(cursor, 8);
    uint64_t status_offset = cursor;
    cursor += sizeof(struct trstat1_status);

    cursor = align_up_u64(cursor, 8);
    uint64_t retained_offset = cursor;
    uint64_t retained_bytes = sizeof(struct trbsr1_header)
        + (uint64_t)MAX_RETAINED_BOOT_SERVICES_RANGES * sizeof(struct trbsr1_entry);
    cursor += retained_bytes;

    uint64_t hii_offset = 0;
    if (hii_len != 0) {
        cursor = align_up_u64(cursor, 8);
        hii_offset = cursor;
        cursor += hii_len;
    }

    uint64_t config_offset = 0;
    if (config_len != 0) {
        cursor = align_up_u64(cursor, 2);
        config_offset = cursor;
        cursor += config_len;
    }

    uint64_t total_bytes = cursor;
    bool ok = total_bytes != 0 && total_bytes <= MAX_PAYLOAD_BYTES;

    void *payload = NULL;
    if (ok) {
        payload = ext_mem_alloc(total_bytes);
        ok = payload != NULL;
    }

    if (ok) {
        memset(payload, 0, total_bytes);

        struct trpay1_header header;
        memset(&header, 0, sizeof(header));
        memcpy(header.magic, "TRPAY1\0\0", 8);
        header.version = TRPAY1_VERSION;
        header.header_bytes = sizeof(header);
        header.section_entry_bytes = sizeof(struct trpay1_section);
        header.section_count = (uint32_t)section_count;
        header.total_bytes = (uint32_t)total_bytes;
        header.capture_flags = status.flags;
        memcpy(payload, &header, sizeof(header));

        struct trpay1_section entries[4];
        memset(entries, 0, sizeof(entries));
        size_t entry_index = 0;

        memcpy((uint8_t *)payload + status_offset, &status, sizeof(status));
        entries[entry_index].kind = SEC_STATUS;
        entries[entry_index].flags = 1;
        entries[entry_index].offset = (uint32_t)status_offset;
        entries[entry_index].length = sizeof(status);
        entries[entry_index].crc32 = crc32_of((uint8_t *)payload + status_offset, sizeof(status));
        entry_index++;

        struct trbsr1_header retained;
        memset(&retained, 0, sizeof(retained));
        memcpy(retained.magic, "TRBSR1\0\0", 8);
        retained.version = 1;
        retained.header_bytes = sizeof(retained);
        retained.entry_bytes = sizeof(struct trbsr1_entry);
        memcpy((uint8_t *)payload + retained_offset, &retained, sizeof(retained));

        size_t retained_entry_index = entry_index;
        entries[entry_index].kind = SEC_BOOT_SERVICES;
        entries[entry_index].offset = (uint32_t)retained_offset;
        entries[entry_index].length = (uint32_t)retained_bytes;
        entries[entry_index].status = (uint64_t)EFI_NOT_READY;
        entries[entry_index].crc32 = crc32_of(
            (uint8_t *)payload + retained_offset,
            (UINTN)retained_bytes
        );
        entry_index++;

        if (hii_len != 0) {
            memcpy((uint8_t *)payload + hii_offset, hii_buffer, hii_len);
            entries[entry_index].kind = SEC_HII;
            entries[entry_index].flags = 1 | (1u << 1); // captured, raw-hii
            entries[entry_index].offset = (uint32_t)hii_offset;
            entries[entry_index].length = (uint32_t)hii_len;
            entries[entry_index].crc32 = crc32_of((uint8_t *)payload + hii_offset, hii_len);
            entries[entry_index].status = status.hii_export_status;
            entry_index++;
        }

        if (config_len != 0) {
            memcpy((uint8_t *)payload + config_offset, config_buffer, config_len);
            entries[entry_index].kind = SEC_CONFIG;
            entries[entry_index].flags = 1 | (1u << 2) | (1u << 3); // captured, utf16, nul-terminated
            entries[entry_index].offset = (uint32_t)config_offset;
            entries[entry_index].length = (uint32_t)config_len;
            entries[entry_index].crc32 = crc32_of((uint8_t *)payload + config_offset, config_len);
            entries[entry_index].status = status.config_export_status;
            entry_index++;
        }

        memcpy((uint8_t *)payload + sizeof(header), entries,
               section_count * sizeof(struct trpay1_section));

        trueos_retained_ranges = (struct trbsr1_header *)((uint8_t *)payload + retained_offset);
        trueos_retained_section = (struct trpay1_section *)(
            (uint8_t *)payload + sizeof(header)
            + retained_entry_index * sizeof(struct trpay1_section)
        );

        *out_address = payload;
        *out_size = total_bytes;
    }

    if (hii_buffer != NULL) {
        gBS->FreePool(hii_buffer);
    }
    if (config_buffer != NULL) {
        gBS->FreePool(config_buffer);
    }

    if (ok) {
        // Arm only after the complete payload exists. The ExitBootServices
        // hook sets bit 31 in this exact header only after it successfully
        // snapshots and protects every Boot Services range and intercepts
        // handoff.
        trueos_capture_flags = &((struct trpay1_header *)payload)->capture_flags;
        if (!trueos_arm_boot_services_retention()) {
            trueos_capture_flags = NULL;
            trueos_retained_ranges = NULL;
            trueos_retained_section = NULL;
        }
    }

    return ok;
}

#endif
