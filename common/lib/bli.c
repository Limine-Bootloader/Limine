#if defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <config.h>
#include <sys/cpu.h>
#include <efi.h>
#include <lib/bli.h>
#include <lib/guid.h>
#include <lib/misc.h>
#include <menu.h>

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

bool decwstr_to_size(const wchar_t *buf, size_t buf_size, size_t *value) {
    size_t i = 0;
    size_t tmp = 0;

    if (buf == NULL) {
        return false;
    }

    if (buf_size == 0 || buf[0] == L'\0') {
        return false;
    }

    while (i * 2 < buf_size && buf[i]) {
        wchar_t c = buf[i];
        if (!(c >= L'0' && c <= L'9')) {
            return false;
        }
        tmp = CHECKED_MUL(tmp, (size_t)10, return false);
        tmp = CHECKED_ADD(tmp, (size_t)(c - L'0'), return false);
        i++;
    }

    *value = tmp;

    return true;
}

void bli_set_loader_time(wchar_t *variable, uint64_t time) {
    if (time == 0)
        return;

    wchar_t time_wstr[21];
    uint64_to_decwstr(time, time_wstr);

    size_t len = 0;
    while (time_wstr[len] != L'\0') len++;

    gRT->SetVariable(variable,
            &bli_vendor_guid,
            EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
            (len + 1) * sizeof(wchar_t),
            time_wstr);
}

void init_bli(void) {
    bli_set_loader_time(L"LoaderTimeInitUSec", usec_at_bootloader_entry);

    gRT->SetVariable(L"LoaderInfo",
            &bli_vendor_guid,
            EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
            sizeof(LIMINE_BRAND),
            LIMINE_BRAND);

    uint64_t features = (1 << 0) | // Timeout control
                        (1 << 1) | // Oneshot timeout control
                        (1 << 2) | // Default entry control
                        (1 << 3) | // Oneshot entry control
                        (1 << 13); // menu-disabled support
    gRT->SetVariable(L"LoaderFeatures",
            &bli_vendor_guid,
            EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
            sizeof(features),
            &features);

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

static bool handle_timeout(wchar_t *variable, bool erase, size_t *timeout, bool *skip_timeout) {
    wchar_t timeout_buf[256];
    UINTN getvar_size = sizeof(timeout_buf) - 2;
    uint32_t attrs;
    if (gRT->GetVariable(variable,
                             &bli_vendor_guid,
                             &attrs,
                             &getvar_size,
                             timeout_buf) == 0 && getvar_size > 0) {
        if (erase) {
            gRT->SetVariable(variable, &bli_vendor_guid,
                attrs,
                0, NULL);
        }
        if (getvar_size == 22 && memcmp(timeout_buf, L"menu-force", 22) == 0) {
            *skip_timeout = true;
            return true;
        }
        if ((getvar_size == 24 && memcmp(timeout_buf, L"menu-hidden",24) == 0) || (getvar_size == 28 && memcmp(timeout_buf, L"menu-disabled",28) == 0)) {
            // TODO: menu-hidden should enable quiet & set timeout >= 1
            *timeout = 0;
            return true;
        }
        size_t t;
        if (!decwstr_to_size(timeout_buf, getvar_size, &t)) {
            return false;
        }
        // For LoaderConfigTimeoutOneShot, "0" means show menu indefinitely.
        if (erase && t == 0) {
            *skip_timeout = true;
            return true;
        }
        *timeout = t;
        return true;
    }
    return false;

}

bool bli_update_oneshot_timeout(size_t *timeout, bool *skip_timeout) {
    return handle_timeout(L"LoaderConfigTimeoutOneShot", true, timeout, skip_timeout);
}

bool bli_update_timeout(size_t *timeout, bool *skip_timeout) {
    return handle_timeout(L"LoaderConfigTimeout", false, timeout, skip_timeout);
}

static bool handle_entry(wchar_t *variable, bool erase, char *path, size_t buf_size) {
    wchar_t wide_path[256];
    UINTN getvar_size = sizeof(wide_path) - 2;
    uint32_t attrs;
    if (gRT->GetVariable(variable,
                             &bli_vendor_guid,
                             &attrs,
                             &getvar_size,
                             wide_path) == 0 && getvar_size > 0) {
        if (erase) {
            gRT->SetVariable(variable, &bli_vendor_guid,
                attrs,
                0, NULL);
        }

        size_t i;
        for (i = 0; i < buf_size-1 && i * 2 < getvar_size; i++) {
            if (wide_path[i] > 0x7f) {
                return false;
            }
            path[i] = wide_path[i] & 0x7f;
        }
        path[i] = 0;

        return true;
    }
    return false;
}

bool bli_get_default_entry(char *path, size_t buf_size) {
    return handle_entry(L"LoaderEntryDefault", false, path, buf_size);
}

bool bli_get_oneshot_entry(char *path, size_t buf_size) {
    return handle_entry(L"LoaderEntryOneShot", true, path, buf_size);
}

void bli_set_selected_entry(const char *path) {
    wchar_t wide_path[MENU_PATH_MAX];
    size_t len = strlen(path);
    if (len > MENU_PATH_MAX - 1) {
        len = MENU_PATH_MAX - 1;
    }
    for (size_t pos = 0; pos < len; pos++) {
        wide_path[pos] = path[pos];
    }
    wide_path[len] = L'\0';
    gRT->SetVariable(L"LoaderEntrySelected",
            &bli_vendor_guid,
            EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
            (len + 1) * sizeof(wchar_t),
            wide_path);
}

#endif
