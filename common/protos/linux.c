#if defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <efi.h>
#include <efi/protocol/efitcg2.h>
#include <protos/linux.h>
#include <lib/misc.h>
#include <lib/tpm.h>
#include <lib/print.h>
#include <lib/libc.h>

#define LINUX_EFI_TPM_EVENT_LOG_GUID \
    { 0xb7799cb0, 0xeca2, 0x4943, { 0x96, 0x67, 0x1f, 0xae, 0x07, 0xb7, 0x47, 0xfa } }

struct linux_efi_tpm_eventlog {
    uint32_t size;
    uint32_t final_events_preboot_size;
    uint8_t  version;
    uint8_t  log[];
} __attribute__((packed));

// Wrap the captured TCG event log in Linux's linux_efi_tpm_eventlog framing
// and publish it as the LINUX_EFI_TPM_EVENT_LOG configuration table for the
// kernel's TPM driver. Limine bypasses the EFI stub that would normally do
// this, and the ACPI TPM2 fallback path the kernel uses otherwise is
// unreliable on common firmware.
void linux_install_efi_tpm_event_log(void) {
    uint32_t format;
    void *log_addr;
    size_t log_size;
    if (!tpm_get_event_log(&format, &log_addr, &log_size)) {
        return;
    }

    // Walk the firmware's final-events table so the kernel can deduplicate
    // any pre-boot events firmware migrated there. The table is selected
    // by the active measurement protocol (TCG2 vs CC).
    uint32_t final_events_preboot_size = 0;
    if (format > EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2) {
        EFI_TCG2_FINAL_EVENTS_TABLE *final_events = tpm_get_final_events_table();
        if (final_events != NULL && final_events->NumberOfEvents > 0) {
            const uint8_t *base = final_events->Events;
            uint64_t remaining = final_events->NumberOfEvents;
            while (remaining > 0) {
                const void *header = base + final_events_preboot_size;
                uint32_t ev_size = tpm_calc_event_size(header, log_addr);
                if (ev_size == 0) {
                    // Malformed entry: a partial sum would skip an arbitrary
                    // prefix and leave the rest looking post-boot, which is
                    // worse than disabling dedup. Hand the kernel 0 so it
                    // processes every final-events entry; the worst case is
                    // duplicate events in its log, not silent loss.
                    printv("linux: malformed entry in TCG final events table; "
                           "disabling preboot dedup\n");
                    final_events_preboot_size = 0;
                    break;
                }
                final_events_preboot_size += ev_size;
                remaining--;
            }
        }
    }

    UINTN total_size = sizeof(struct linux_efi_tpm_eventlog) + log_size;
    struct linux_efi_tpm_eventlog *log_tbl = NULL;
    EFI_STATUS status = gBS->AllocatePool(EfiACPIReclaimMemory, total_size,
                                          (void **)&log_tbl);
    if (status != EFI_SUCCESS) {
        printv("linux: failed to allocate event log table: %X\n", (uint64_t)status);
        return;
    }

    memset(log_tbl, 0, total_size);
    log_tbl->size = (uint32_t)log_size;
    log_tbl->final_events_preboot_size = final_events_preboot_size;
    log_tbl->version = (uint8_t)format;
    if (log_size > 0) {
        memcpy(log_tbl->log, log_addr, log_size);
    }

    EFI_GUID linux_log_guid = LINUX_EFI_TPM_EVENT_LOG_GUID;
    status = gBS->InstallConfigurationTable(&linux_log_guid, log_tbl);
    if (status != EFI_SUCCESS) {
        printv("linux: failed to install event log table: %X\n", (uint64_t)status);
        gBS->FreePool(log_tbl);
        return;
    }

    tpm_release_event_log();

    printv("linux: installed event log (%u bytes, format TCG_%s) as configuration table\n",
           log_tbl->size,
           format == EFI_TCG2_EVENT_LOG_FORMAT_TCG_2 ? "2" : "1.2");
}

#endif
