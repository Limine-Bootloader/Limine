#if defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <efi.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/trueos_hii.h>
#include <lib/trueos_firmware_bridge.h>
#include <mm/pmm.h>

#define TRPAY1_VERSION 1

#define SEC_STATUS           1
#define SEC_HII              2
#define SEC_CONFIG           3
#define SEC_BOOT_SERVICES    4
#define SEC_FIRMWARE_CONTEXT 5

#define CAPTURE_FLAG_HII_DATABASE           (1u << 0)
#define CAPTURE_FLAG_HII_PACKAGES           (1u << 1)
#define CAPTURE_FLAG_CONFIG_ROUTING         (1u << 3)
#define CAPTURE_FLAG_CONFIG                 (1u << 4)
#define CAPTURE_FLAG_BOOT_SERVICES_RETAINED (1u << 31)

#define MAX_HII_PACKAGE_BYTES (12u * 1024u * 1024u)
#define MAX_HII_CONFIG_BYTES  (4u * 1024u * 1024u)
#define MAX_PAYLOAD_BYTES     (16u * 1024u * 1024u)
#define MAX_RETAINED_BOOT_SERVICES_RANGES 512u
#define MAX_FIRMWARE_PAGE_TABLE_PAGES 4096u
#define MAX_BOOT_SERVICE_ENTRYPOINTS 128u
#define MAX_BRIDGE_STACK_PAGES 32u
#define TRUEOS_FW_BRIDGE_STACK_BYTES (64u * 1024u)

#define BOOT_SERVICES_RANGE_EXECUTABLE  (1u << 0)
#define BOOT_SERVICES_RANGE_ENTRYPOINT   (1u << 1)
#define BOOT_SERVICES_RANGE_TABLE        (1u << 2)
#define BOOT_SERVICES_RANGE_PAGE_TABLE   (1u << 3)
#define BOOT_SERVICES_RANGE_BRIDGE_CODE  (1u << 4)
#define BOOT_SERVICES_RANGE_BRIDGE_DATA  (1u << 5)

#define BOOT_SERVICES_SET_COMPLETE          (1u << 0)
#define BOOT_SERVICES_SET_WATCHDOG_DISABLED (1u << 1)
#define BOOT_SERVICES_SET_EXIT_GROUP_SENT   (1u << 2)
#define BOOT_SERVICES_SET_CONTEXT_READY     (1u << 3)

#define FW_CONTEXT_COMPLETE          (1u << 0)
#define FW_CONTEXT_QUIESCED          (1u << 1)
#define FW_CONTEXT_CR3_RETAINED      (1u << 2)
#define FW_CONTEXT_BRIDGE_READY      (1u << 3)
#define FW_CONTEXT_WATCHDOG_DISABLED (1u << 4)
#define FW_CONTEXT_EXIT_GROUP_SENT   (1u << 5)

#define FW_STAGE_NONE          0u
#define FW_STAGE_BRIDGE_ALLOC  1u
#define FW_STAGE_QUIESCE       2u
#define FW_STAGE_TRANSLATE     3u
#define FW_STAGE_PAGE_TABLES   4u
#define FW_STAGE_RETAIN_MAP    5u
#define FW_STAGE_TABLE_CRC     6u

#define TRUEOS_HII_DATABASE_PROTOCOL_GUID \
    { 0xef9fc172, 0xa1b2, 0x4693, { 0xb3, 0x27, 0x6d, 0x32, 0xfc, 0x41, 0x60, 0x42 } }
#define TRUEOS_HII_CONFIG_ROUTING_PROTOCOL_GUID \
    { 0x587e72d7, 0xcc50, 0x4f79, { 0x82, 0x09, 0xca, 0x29, 0x1f, 0xc1, 0xa1, 0x0f } }
#define TRUEOS_EXIT_BOOT_SERVICES_EVENT_GROUP_GUID \
    { 0x27abf055, 0xb1b8, 0x4c26, { 0x80, 0x48, 0x74, 0x8f, 0x37, 0xba, 0xa2, 0xdf } }

#define PT_PRESENT ((uint64_t)1 << 0)
#define PT_HUGE    ((uint64_t)1 << 7)
#define PT_ADDR_MASK ((uint64_t)0x000FFFFFFFFFF000)
#define CR4_LA57   ((uint64_t)1 << 12)

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

struct trfwc1_context {
    uint8_t  magic[8];
    uint16_t version;
    uint16_t bytes;
    uint32_t flags;
    uint32_t failure_stage;
    uint32_t reserved0;
    uint64_t firmware_cr3;
    uint64_t firmware_cr4;
    uint64_t boot_services_virtual;
    uint64_t boot_services_physical;
    uint64_t calculate_crc32_virtual;
    uint64_t calculate_crc32_physical;
    uint64_t bridge_entry_virtual;
    uint64_t bridge_entry_physical;
    uint32_t bridge_entry_bytes;
    uint32_t bridge_control_bytes;
    uint64_t bridge_control_virtual;
    uint64_t bridge_control_physical;
    uint64_t bridge_stack_base_virtual;
    uint32_t bridge_stack_bytes;
    uint32_t page_table_pages;
};
#pragma pack(pop)

typedef EFI_STATUS (EFIAPI *TRUEOS_EXIT_BOOT_SERVICES)(EFI_HANDLE ImageHandle, UINTN MapKey);

static TRUEOS_EXIT_BOOT_SERVICES trueos_original_exit_boot_services = NULL;
static uint32_t *trueos_capture_flags = NULL;
static struct trbsr1_header *trueos_retained_ranges = NULL;
static struct trpay1_section *trueos_retained_section = NULL;
static struct trfwc1_context *trueos_firmware_context = NULL;
static struct trpay1_section *trueos_context_section = NULL;
static struct trueos_firmware_bridge_control *trueos_bridge_control = NULL;
static uint8_t *trueos_bridge_stack = NULL;
static bool trueos_quiesce_completed = false;
static bool trueos_watchdog_disabled = false;
static bool trueos_exit_group_signalled = false;

#if defined (__x86_64__)
static uint64_t trueos_page_table_pages[MAX_FIRMWARE_PAGE_TABLE_PAGES];
static size_t trueos_page_table_page_count = 0;
static uint64_t trueos_entrypoint_phys[MAX_BOOT_SERVICE_ENTRYPOINTS];
static size_t trueos_entrypoint_phys_count = 0;
static uint64_t trueos_bridge_stack_phys[MAX_BRIDGE_STACK_PAGES];
static size_t trueos_bridge_stack_phys_count = 0;
#endif

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

static bool trueos_descriptor_contains_phys(EFI_MEMORY_DESCRIPTOR *entry, uint64_t address) {
    uint64_t base, top;
    if (!trueos_descriptor_bounds(entry, &base, &top)) {
        return false;
    }
    return address >= base && address < top;
}

#if defined (__x86_64__)
static uint64_t trueos_read_cr3(void) {
    uint64_t value;
    asm volatile ("mov %%cr3, %0" : "=r"(value));
    return value;
}

static uint64_t trueos_read_cr4(void) {
    uint64_t value;
    asm volatile ("mov %%cr4, %0" : "=r"(value));
    return value;
}

static bool trueos_firmware_translate(uint64_t virt, uint64_t *phys_out) {
    uint64_t cr3 = trueos_read_cr3();
    uint64_t cr4 = trueos_read_cr4();
    int levels = (cr4 & CR4_LA57) ? 5 : 4;
    uint64_t table_phys = cr3 & PT_ADDR_MASK;

    for (int level = levels; level >= 1; level--) {
        uint64_t shift = 12 + (uint64_t)(level - 1) * 9;
        size_t index = (size_t)((virt >> shift) & 0x1ff);
        uint64_t *table = (uint64_t *)(uintptr_t)table_phys;
        uint64_t entry = table[index];
        if (!(entry & PT_PRESENT)) {
            return false;
        }

        if (level == 1) {
            *phys_out = (entry & PT_ADDR_MASK) + (virt & 0xfff);
            return true;
        }

        if ((level == 3 || level == 2) && (entry & PT_HUGE)) {
            uint64_t span = (uint64_t)1 << shift;
            uint64_t base = (entry & PT_ADDR_MASK) & ~(span - 1);
            *phys_out = base + (virt & (span - 1));
            return true;
        }

        table_phys = entry & PT_ADDR_MASK;
    }

    return false;
}

static bool trueos_add_unique(uint64_t *items, size_t *count, size_t capacity, uint64_t value) {
    for (size_t i = 0; i < *count; i++) {
        if (items[i] == value) {
            return true;
        }
    }
    if (*count == capacity) {
        return false;
    }
    items[(*count)++] = value;
    return true;
}

static bool trueos_collect_page_table(uint64_t table_phys, int level) {
    table_phys &= PT_ADDR_MASK;
    if (!trueos_add_unique(
            trueos_page_table_pages,
            &trueos_page_table_page_count,
            MAX_FIRMWARE_PAGE_TABLE_PAGES,
            table_phys)) {
        return false;
    }
    if (level <= 1) {
        return true;
    }

    uint64_t *table = (uint64_t *)(uintptr_t)table_phys;
    for (size_t i = 0; i < 512; i++) {
        uint64_t entry = table[i];
        if (!(entry & PT_PRESENT)) {
            continue;
        }
        if ((level == 3 || level == 2) && (entry & PT_HUGE)) {
            continue;
        }
        if (!trueos_collect_page_table(entry & PT_ADDR_MASK, level - 1)) {
            return false;
        }
    }
    return true;
}

static bool trueos_collect_firmware_page_tables(void) {
    trueos_page_table_page_count = 0;
    int levels = (trueos_read_cr4() & CR4_LA57) ? 5 : 4;
    return trueos_collect_page_table(trueos_read_cr3() & PT_ADDR_MASK, levels);
}

static bool trueos_collect_entrypoints(void) {
    trueos_entrypoint_phys_count = 0;
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
        if (target == 0) {
            continue;
        }
        uint64_t phys = 0;
        if (!trueos_firmware_translate((uint64_t)target, &phys)) {
            return false;
        }
        if (!trueos_add_unique(
                trueos_entrypoint_phys,
                &trueos_entrypoint_phys_count,
                MAX_BOOT_SERVICE_ENTRYPOINTS,
                phys)) {
            return false;
        }
    }

    if (trueos_original_exit_boot_services != NULL) {
        uint64_t phys = 0;
        if (!trueos_firmware_translate((uint64_t)(uintptr_t)trueos_original_exit_boot_services, &phys)) {
            return false;
        }
        if (!trueos_add_unique(
                trueos_entrypoint_phys,
                &trueos_entrypoint_phys_count,
                MAX_BOOT_SERVICE_ENTRYPOINTS,
                phys)) {
            return false;
        }
    }
    return true;
}

static bool trueos_collect_bridge_stack_pages(void) {
    trueos_bridge_stack_phys_count = 0;
    if (trueos_bridge_stack == NULL) {
        return false;
    }

    uint64_t start = (uint64_t)(uintptr_t)trueos_bridge_stack;
    uint64_t end = start + TRUEOS_FW_BRIDGE_STACK_BYTES;
    uint64_t page = start & ~(uint64_t)0xfff;
    uint64_t top = (end + 0xfff) & ~(uint64_t)0xfff;
    for (; page < top; page += 0x1000) {
        uint64_t phys = 0;
        if (!trueos_firmware_translate(page, &phys)) {
            return false;
        }
        phys &= ~(uint64_t)0xfff;
        if (!trueos_add_unique(
                trueos_bridge_stack_phys,
                &trueos_bridge_stack_phys_count,
                MAX_BRIDGE_STACK_PAGES,
                phys)) {
            return false;
        }
    }
    return true;
}
#endif

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

static void trueos_refresh_context_section(EFI_STATUS status) {
    if (trueos_firmware_context == NULL || trueos_context_section == NULL || gBS == NULL) {
        return;
    }
    trueos_context_section->status = (uint64_t)status;
    UINT32 crc = 0;
    EFI_STATUS crc_status = gBS->CalculateCrc32(
        trueos_firmware_context,
        trueos_context_section->length,
        &crc
    );
    if (!EFI_ERROR(crc_status)) {
        trueos_context_section->crc32 = crc;
    }
}

static void trueos_set_failure(uint32_t stage, EFI_STATUS status) {
    if (trueos_firmware_context != NULL) {
        trueos_firmware_context->failure_stage = stage;
        if (trueos_quiesce_completed) {
            trueos_firmware_context->flags |= FW_CONTEXT_QUIESCED;
        }
        if (trueos_watchdog_disabled) {
            trueos_firmware_context->flags |= FW_CONTEXT_WATCHDOG_DISABLED;
        }
        if (trueos_exit_group_signalled) {
            trueos_firmware_context->flags |= FW_CONTEXT_EXIT_GROUP_SENT;
        }
    }
    trueos_refresh_context_section(status);
}

static bool trueos_prepare_bridge_resources(void) {
#if defined (__x86_64__)
    if (sizeof(struct trueos_firmware_bridge_control) != TRUEOS_FW_BRIDGE_CONTROL_BYTES) {
        return false;
    }
    if (trueos_bridge_control == NULL) {
        void *control = NULL;
        EFI_STATUS alloc = gBS->AllocatePool( EfiLoaderData, 4096, &control);
        if (EFI_ERROR(alloc) || control == NULL) {
            return false;
        }
        memset(control, 0, 4096);
        trueos_bridge_control = control;
    }
    if (trueos_bridge_stack == NULL) {
        void *stack = NULL;
        EFI_STATUS alloc = gBS->AllocatePool(EfiLoaderData, TRUEOS_FW_BRIDGE_STACK_BYTES, &stack);
        if (EFI_ERROR(alloc) || stack == NULL) {
            return false;
        }
        memset(stack, 0, TRUEOS_FW_BRIDGE_STACK_BYTES);
        trueos_bridge_stack = stack;
    }
    return true;
#else
    return false;
#endif
}

static bool trueos_prepare_firmware_context(void) {
#if defined (__x86_64__)
    if (trueos_firmware_context == NULL || trueos_bridge_control == NULL
     || trueos_bridge_stack == NULL || gBS == NULL || gBS->CalculateCrc32 == NULL) {
        return false;
    }

    uint64_t gbs_phys = 0;
    uint64_t crc_phys = 0;
    uint64_t bridge_phys = 0;
    uint64_t control_phys = 0;
    uint64_t bridge_va = (uint64_t)(uintptr_t)trueos_firmware_bridge_entry;
    uint64_t bridge_end = (uint64_t)(uintptr_t)trueos_firmware_bridge_end;
    uint64_t bridge_bytes = bridge_end > bridge_va ? bridge_end - bridge_va : 0;
    if (bridge_bytes == 0 || bridge_bytes > 4096
     || (bridge_va & ~(uint64_t)0xfff) != ((bridge_end - 1) & ~(uint64_t)0xfff)) {
        return false;
    }

    if (!trueos_firmware_translate((uint64_t)(uintptr_t)gBS, &gbs_phys)
     || !trueos_firmware_translate((uint64_t)(uintptr_t)gBS->CalculateCrc32, &crc_phys)
     || !trueos_firmware_translate(bridge_va, &bridge_phys)
     || !trueos_firmware_translate((uint64_t)(uintptr_t)trueos_bridge_control, &control_phys)
     || !trueos_collect_entrypoints()
     || !trueos_collect_bridge_stack_pages()) {
        return false;
    }

    if (!trueos_collect_firmware_page_tables()) {
        return false;
    }

    memset(trueos_bridge_control, 0, sizeof(*trueos_bridge_control));
    trueos_bridge_control->firmware_cr3 = trueos_read_cr3();
    uint64_t stack_top = (uint64_t)(uintptr_t)trueos_bridge_stack + TRUEOS_FW_BRIDGE_STACK_BYTES;
    trueos_bridge_control->firmware_stack_top = stack_top & ~(uint64_t)0xf;

    memcpy(trueos_firmware_context->magic, "TRFWC1\0\0", 8);
    trueos_firmware_context->version = 1;
    trueos_firmware_context->bytes = sizeof(*trueos_firmware_context);
    trueos_firmware_context->flags = FW_CONTEXT_QUIESCED;
    if (trueos_watchdog_disabled) {
        trueos_firmware_context->flags |= FW_CONTEXT_WATCHDOG_DISABLED;
    }
    if (trueos_exit_group_signalled) {
        trueos_firmware_context->flags |= FW_CONTEXT_EXIT_GROUP_SENT;
    }
    trueos_firmware_context->failure_stage = FW_STAGE_NONE;
    trueos_firmware_context->firmware_cr3 = trueos_read_cr3();
    trueos_firmware_context->firmware_cr4 = trueos_read_cr4();
    trueos_firmware_context->boot_services_virtual = (uint64_t)(uintptr_t)gBS;
    trueos_firmware_context->boot_services_physical = gbs_phys;
    trueos_firmware_context->calculate_crc32_virtual = (uint64_t)(uintptr_t)gBS->CalculateCrc32;
    trueos_firmware_context->calculate_crc32_physical = crc_phys;
    trueos_firmware_context->bridge_entry_virtual = bridge_va;
    trueos_firmware_context->bridge_entry_physical = bridge_phys;
    trueos_firmware_context->bridge_entry_bytes = (uint32_t)bridge_bytes;
    trueos_firmware_context->bridge_control_bytes = sizeof(*trueos_bridge_control);
    trueos_firmware_context->bridge_control_virtual = (uint64_t)(uintptr_t)trueos_bridge_control;
    trueos_firmware_context->bridge_control_physical = control_phys;
    trueos_firmware_context->bridge_stack_base_virtual = (uint64_t)(uintptr_t)trueos_bridge_stack;
    trueos_firmware_context->bridge_stack_bytes = TRUEOS_FW_BRIDGE_STACK_BYTES;
    trueos_firmware_context->page_table_pages = (uint32_t)trueos_page_table_page_count;
    return true;
#else
    return false;
#endif
}

#if defined (__x86_64__)
static bool trueos_descriptor_contains_any(
    EFI_MEMORY_DESCRIPTOR *entry,
    const uint64_t *items,
    size_t count
) {
    for (size_t i = 0; i < count; i++) {
        if (trueos_descriptor_contains_phys(entry, items[i])) {
            return true;
        }
    }
    return false;
}

static uint32_t trueos_descriptor_roles(EFI_MEMORY_DESCRIPTOR *entry) {
    uint32_t roles = 0;
    if (entry->Type == EfiBootServicesCode) {
        roles |= BOOT_SERVICES_RANGE_EXECUTABLE;
    }
    if (entry->Type == EfiBootServicesCode || entry->Type == EfiBootServicesData) {
        roles |= 0x80000000u; // internal: retain standard Boot Services descriptor
    }
    if (trueos_descriptor_contains_any(entry, trueos_entrypoint_phys, trueos_entrypoint_phys_count)) {
        roles |= BOOT_SERVICES_RANGE_ENTRYPOINT | BOOT_SERVICES_RANGE_EXECUTABLE;
    }
    if (trueos_firmware_context != NULL
     && trueos_descriptor_contains_phys(entry, trueos_firmware_context->boot_services_physical)) {
        roles |= BOOT_SERVICES_RANGE_TABLE;
    }
    if (trueos_descriptor_contains_any(entry, trueos_page_table_pages, trueos_page_table_page_count)) {
        roles |= BOOT_SERVICES_RANGE_PAGE_TABLE;
    }
    if (trueos_firmware_context != NULL
     && trueos_descriptor_contains_phys(entry, trueos_firmware_context->bridge_entry_physical)) {
        roles |= BOOT_SERVICES_RANGE_BRIDGE_CODE | BOOT_SERVICES_RANGE_EXECUTABLE;
    }
    if (trueos_firmware_context != NULL
     && (trueos_descriptor_contains_phys(entry, trueos_firmware_context->bridge_control_physical)
      || trueos_descriptor_contains_any(entry, trueos_bridge_stack_phys, trueos_bridge_stack_phys_count))) {
        roles |= BOOT_SERVICES_RANGE_BRIDGE_DATA;
    }
    return roles;
}
#endif

static bool trueos_protect_boot_services_in_limine_map(void) {
#if defined (__x86_64__)
    if (efi_mmap == NULL || efi_desc_size < sizeof(EFI_MEMORY_DESCRIPTOR) || efi_desc_size == 0
     || trueos_retained_ranges == NULL || trueos_retained_section == NULL
     || trueos_firmware_context == NULL || gBS == NULL || gBS->CalculateCrc32 == NULL) {
        return false;
    }

    UINTN count = efi_mmap_size / efi_desc_size;
    UINTN retained_count = 0;
    bool table_found = false;
    bool crc_found = false;
    bool bridge_code_found = false;
    bool bridge_data_found = false;
    size_t page_table_pages_covered = 0;

    for (UINTN i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR *entry = (void *)((uint8_t *)efi_mmap + i * efi_desc_size);
        if (entry->NumberOfPages == 0) {
            continue;
        }
        uint32_t roles = trueos_descriptor_roles(entry);
        if (roles == 0) {
            continue;
        }
        if (retained_count == MAX_RETAINED_BOOT_SERVICES_RANGES) {
            return false;
        }
        retained_count++;
        table_found |= !!(roles & BOOT_SERVICES_RANGE_TABLE);
        bridge_code_found |= !!(roles & BOOT_SERVICES_RANGE_BRIDGE_CODE);
        bridge_data_found |= !!(roles & BOOT_SERVICES_RANGE_BRIDGE_DATA);
        if (roles & BOOT_SERVICES_RANGE_ENTRYPOINT
         && trueos_descriptor_contains_phys(entry, trueos_firmware_context->calculate_crc32_physical)) {
            crc_found = true;
        }
        for (size_t p = 0; p < trueos_page_table_page_count; p++) {
            if (trueos_descriptor_contains_phys(entry, trueos_page_table_pages[p])) {
                page_table_pages_covered++;
            }
        }
    }

    if (retained_count == 0 || !table_found || !crc_found
     || !bridge_code_found || !bridge_data_found
     || page_table_pages_covered < trueos_page_table_page_count) {
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
        uint32_t roles = trueos_descriptor_roles(entry);
        if (roles == 0) {
            continue;
        }

        ranges[out].physical_start = entry->PhysicalStart;
        ranges[out].length = (uint64_t)entry->NumberOfPages * 4096;
        ranges[out].memory_type = entry->Type;
        ranges[out].flags = roles & ~0x80000000u;
        out++;
        entry->Type = EfiReservedMemoryType;
    }

    trueos_retained_ranges->count = (uint32_t)out;
    trueos_retained_ranges->flags = BOOT_SERVICES_SET_COMPLETE | BOOT_SERVICES_SET_CONTEXT_READY;
    if (trueos_watchdog_disabled) {
        trueos_retained_ranges->flags |= BOOT_SERVICES_SET_WATCHDOG_DISABLED;
    }
    if (trueos_exit_group_signalled) {
        trueos_retained_ranges->flags |= BOOT_SERVICES_SET_EXIT_GROUP_SENT;
    }
    trueos_retained_section->flags = 1;
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
#else
    return false;
#endif
}

static EFI_STATUS EFIAPI trueos_retain_exit_boot_services(EFI_HANDLE image_handle, UINTN map_key) {
    TRUEOS_EXIT_BOOT_SERVICES original = trueos_original_exit_boot_services;

    if (!trueos_quiesce_completed) {
        if (!trueos_prepare_firmware_quiesce()) {
            trueos_set_failure(FW_STAGE_QUIESCE, EFI_ABORTED);
            if (original != NULL) {
                gBS->ExitBootServices = original;
                (void)trueos_refresh_boot_services_crc();
                return original(image_handle, map_key);
            }
            return EFI_ABORTED;
        }
        if (trueos_firmware_context != NULL) {
            trueos_firmware_context->flags |= FW_CONTEXT_QUIESCED | FW_CONTEXT_EXIT_GROUP_SENT;
            if (trueos_watchdog_disabled) {
                trueos_firmware_context->flags |= FW_CONTEXT_WATCHDOG_DISABLED;
            }
            trueos_firmware_context->failure_stage = FW_STAGE_NONE;
            trueos_refresh_context_section(EFI_NOT_READY);
        }
        return EFI_INVALID_PARAMETER;
    }

    if (!trueos_prepare_firmware_context()) {
        trueos_set_failure(FW_STAGE_TRANSLATE, EFI_NOT_READY);
        if (original != NULL) {
            gBS->ExitBootServices = original;
            (void)trueos_refresh_boot_services_crc();
            return original(image_handle, map_key);
        }
        return EFI_ABORTED;
    }

    if (!trueos_protect_boot_services_in_limine_map()) {
        trueos_set_failure(FW_STAGE_RETAIN_MAP, EFI_NOT_READY);
        if (original != NULL) {
            gBS->ExitBootServices = original;
            (void)trueos_refresh_boot_services_crc();
            return original(image_handle, map_key);
        }
        return EFI_ABORTED;
    }

    if (original != NULL) {
        gBS->ExitBootServices = original;
    }
    bool table_crc_ok = trueos_refresh_boot_services_crc();
    if (!table_crc_ok || trueos_capture_flags == NULL || original == NULL) {
        trueos_set_failure(FW_STAGE_TABLE_CRC, EFI_ABORTED);
        if (original != NULL) {
            return original(image_handle, map_key);
        }
        return EFI_ABORTED;
    }

    trueos_firmware_context->flags |=
        FW_CONTEXT_COMPLETE | FW_CONTEXT_CR3_RETAINED | FW_CONTEXT_BRIDGE_READY;
    trueos_firmware_context->failure_stage = FW_STAGE_NONE;
    trueos_refresh_context_section(EFI_SUCCESS);
    *trueos_capture_flags |= CAPTURE_FLAG_BOOT_SERVICES_RETAINED;
    return EFI_SUCCESS;
}

static bool trueos_arm_boot_services_retention(void) {
    if (trueos_original_exit_boot_services != NULL) {
        return true;
    }
    if (gBS == NULL || gBS->ExitBootServices == NULL || !trueos_prepare_bridge_resources()) {
        trueos_set_failure(FW_STAGE_BRIDGE_ALLOC, EFI_NOT_READY);
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
    trueos_firmware_context = NULL;
    trueos_context_section = NULL;
    trueos_bridge_control = NULL;
    trueos_bridge_stack = NULL;
    trueos_quiesce_completed = false;
    trueos_watchdog_disabled = false;
    trueos_exit_group_signalled = false;
#if defined (__x86_64__)
    trueos_page_table_page_count = 0;
    trueos_entrypoint_phys_count = 0;
    trueos_bridge_stack_phys_count = 0;
#endif

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

    size_t section_count = 3 + (hii_len != 0 ? 1 : 0) + (config_len != 0 ? 1 : 0);
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

    cursor = align_up_u64(cursor, 8);
    uint64_t context_offset = cursor;
    cursor += sizeof(struct trfwc1_context);

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

        struct trpay1_section entries[5];
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
        entries[entry_index].crc32 = crc32_of((uint8_t *)payload + retained_offset, (UINTN)retained_bytes);
        entry_index++;

        struct trfwc1_context context;
        memset(&context, 0, sizeof(context));
        memcpy(context.magic, "TRFWC1\0\0", 8);
        context.version = 1;
        context.bytes = sizeof(context);
        context.failure_stage = FW_STAGE_BRIDGE_ALLOC;
        memcpy((uint8_t *)payload + context_offset, &context, sizeof(context));

        size_t context_entry_index = entry_index;
        entries[entry_index].kind = SEC_FIRMWARE_CONTEXT;
        entries[entry_index].offset = (uint32_t)context_offset;
        entries[entry_index].length = sizeof(context);
        entries[entry_index].status = (uint64_t)EFI_NOT_READY;
        entries[entry_index].crc32 = crc32_of((uint8_t *)payload + context_offset, sizeof(context));
        entry_index++;

        if (hii_len != 0) {
            memcpy((uint8_t *)payload + hii_offset, hii_buffer, hii_len);
            entries[entry_index].kind = SEC_HII;
            entries[entry_index].flags = 1 | (1u << 1);
            entries[entry_index].offset = (uint32_t)hii_offset;
            entries[entry_index].length = (uint32_t)hii_len;
            entries[entry_index].crc32 = crc32_of((uint8_t *)payload + hii_offset, hii_len);
            entries[entry_index].status = status.hii_export_status;
            entry_index++;
        }

        if (config_len != 0) {
            memcpy((uint8_t *)payload + config_offset, config_buffer, config_len);
            entries[entry_index].kind = SEC_CONFIG;
            entries[entry_index].flags = 1 | (1u << 2) | (1u << 3);
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
            + retained_entry_index * sizeof(struct trpay1_section));
        trueos_firmware_context = (struct trfwc1_context *)((uint8_t *)payload + context_offset);
        trueos_context_section = (struct trpay1_section *)(
            (uint8_t *)payload + sizeof(header)
            + context_entry_index * sizeof(struct trpay1_section));

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
