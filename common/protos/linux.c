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

struct linux_efi_tpm_event_log {
    uint32_t size;
    uint32_t final_events_preboot_size;
    uint8_t version;
    uint8_t log[];
} __attribute__((packed));

void linux_install_efi_tpm_event_log(void) {
    if (!tpm_present()) {
        return;
    }

    uint32_t format;
    void *log_data;
    size_t log_size;
    if (!tpm_get_event_log(&format, &log_data, &log_size)) {
        printv("linux: No TPM event log to pass on\n");
        return;
    }

    size_t preboot_size = tpm_get_final_events_preboot_size();
    struct linux_efi_tpm_event_log *table = NULL;
    EFI_GUID guid = LINUX_EFI_TPM_EVENT_LOG_GUID;
    EFI_STATUS status;

    if (log_size == 0) {
        printv("linux: TPM event log is empty, not passing it on\n");
        goto out;
    }
    // The pre-boot events are part of the log, so both sizes fit the fields.
    if (log_size > UINT32_MAX - sizeof(*table) || preboot_size > log_size) {
        printv("linux: TPM event log is too large to pass on\n");
        goto out;
    }

    // The kernel reads the table after ExitBootServices(), and memory of this
    // type is preserved until ACPI is enabled (UEFI 2.11 section 7.2).
    status = gBS->AllocatePool(EfiACPIReclaimMemory, sizeof(*table) + log_size,
                               (void **)&table);
    if (status != EFI_SUCCESS) {
        printv("linux: Failed to allocate the TPM event log table: %X\n", (uint64_t)status);
        goto out;
    }

    table->size = log_size;
    table->final_events_preboot_size = preboot_size;
    table->version = format;
    memcpy(table->log, log_data, log_size);

    status = gBS->InstallConfigurationTable(&guid, table);
    if (status != EFI_SUCCESS) {
        printv("linux: Failed to install the TPM event log table: %X\n", (uint64_t)status);
        gBS->FreePool(table);
        goto out;
    }

    printv("linux: Installed TPM event log table at %p\n", table);

out:
    tpm_release_event_log();
}

#endif
