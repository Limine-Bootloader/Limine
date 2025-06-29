#if defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <config.h>
#include <sys/cpu.h>
#include <efi.h>
#include <lib/bli.h>
#include <lib/guid.h>
#include <lib/misc.h>

#define LIMINE_BRAND L"Limine " LIMINE_VERSION

static EFI_GUID bli_vendor_guid = { 0x4a67b082, 0x0a4c, 0x41cf, { 0xb6, 0xc7, 0x44, 0x0b, 0x29, 0xbb, 0x8c, 0x4f } };

// The buffer must be at least 21 bytes long
void uint64_to_decwstr(uint64_t value, wchar_t *buf) {
    wchar_t tmp[21];
    size_t i = 0;

    if (buf == NULL) {
        return;
    }

    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }

    // Convert digits in reverse order
    while (value > 0) {
        tmp[i++] = '0' + (value % 10);
        value /= 10;
    }

    // Reverse the string into the buffer
    for (size_t j = 0; j < i; j++) {
        buf[j] = tmp[i - j - 1];
    }
    buf[i] = '\0';
}

void bli_set_loader_time(wchar_t *variable, uint64_t time) {
    if (time == 0)
        return;

    wchar_t time_wstr[21];
    uint64_to_decwstr(time, time_wstr);

    gRT->SetVariable(variable,
            &bli_vendor_guid,
            EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
            sizeof(time_wstr),
            time_wstr);
}

void init_bli(void) {
    bli_set_loader_time(L"LoaderTimeInitUSec", usec_at_bootloader_entry);

    gRT->SetVariable(L"LoaderInfo",
            &bli_vendor_guid,
            EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
            sizeof(LIMINE_BRAND),
            LIMINE_BRAND);

    char part_uuid_str[37];
    guid_to_string(&boot_volume->part_guid, part_uuid_str);

    // Convert part_uuid_str to a wide-char string
    wchar_t part_uuid[37];
    for (size_t i = 0; i < 37; i++) {
        part_uuid[i] = (wchar_t) part_uuid_str[i];
    }

    gRT->SetVariable(L"LoaderDevicePartUUID",
            &bli_vendor_guid,
            EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
            sizeof(part_uuid),
            part_uuid);
}

void bli_on_boot(void) {
    bli_set_loader_time(L"LoaderTimeExecUSec", rdtsc_usec());
}

#endif
