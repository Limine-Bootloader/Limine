#include <protos/efi_boot_entry.h>

#if defined(UEFI)

#include <mm/pmm.h>
#include <efi.h>
#include <lib/config.h>
#include <lib/misc.h>
#include <stdbool.h>

static bool uefi_string_matches(CHAR16 *desc, size_t desc_max_chars, CHAR16 *target) {
    while (*target) {
        if (desc_max_chars == 0)
            return false;
        CHAR16 a = *desc >= L'a' && *desc <= L'z' ? *desc - 32 : *desc;
        CHAR16 b = *target >= L'a' && *target <= L'z' ? *target - 32 : *target;
        if (a != b)
            return false;
        desc++;
        target++;
        desc_max_chars--;
    }
    return desc_max_chars > 0 && *desc == L'\0';
}

static void format_boot_var(CHAR16 *out, UINT16 num) {
    out[0] = L'B';
    out[1] = L'o';
    out[2] = L'o';
    out[3] = L't';
    out[4] = L"0123456789ABCDEF"[(num >> 12) & 0xF];
    out[5] = L"0123456789ABCDEF"[(num >> 8) & 0xF];
    out[6] = L"0123456789ABCDEF"[(num >> 4) & 0xF];
    out[7] = L"0123456789ABCDEF"[(num >> 0) & 0xF];
    out[8] = L'\0';
}

static bool find_boot_entry(CHAR16 *entry, uint16_t *out) {
    EFI_STATUS status;
    uint16_t boot_order[128];
    size_t size = sizeof(boot_order);
    EFI_GUID global_variable = EFI_GLOBAL_VARIABLE;

    status = gRT->GetVariable(L"BootOrder", &global_variable, NULL, &size, boot_order);

    if (EFI_ERROR(status)) {
        panic(true, "efi_boot_entry: Failed to get BootOrder variable (%X)",
            (uint64_t)status);
    }

    size_t count = size / sizeof(uint16_t);

    for (size_t i = 0; i < count; i++) {
        CHAR16 var_name[9];

        format_boot_var(var_name, boot_order[i]);

        size_t buf_size = 0;
        /* Query for buf size */
        gRT->GetVariable(var_name, &global_variable, NULL, &buf_size, NULL);
        uint8_t *buf = ext_mem_alloc(buf_size);
        status = gRT->GetVariable(var_name, &global_variable, NULL, &buf_size, buf);

        if (EFI_ERROR(status)) {
            pmm_free(buf, buf_size);
            continue; 
        }
        /* Get the description */
        if (buf_size < sizeof(uint32_t) + sizeof(uint16_t) + sizeof(CHAR16)) {
            pmm_free(buf, buf_size);
            continue;
        }
        size_t desc_offset = sizeof(uint32_t) + sizeof(uint16_t);
        CHAR16 *desc = (CHAR16 *)(buf + desc_offset);
        size_t desc_max_chars = (buf_size - desc_offset) / sizeof(CHAR16);
        if (uefi_string_matches(desc, desc_max_chars, entry)) {
            *out = boot_order[i];
            pmm_free(buf, buf_size);
            return true;
        }
        pmm_free(buf, buf_size);
    }

    return false;
}

noreturn void efi_boot_entry(char *config) {
    char *boot_entry = config_get_value(config, 0, "ENTRY");

    if (boot_entry == NULL) {
        panic(true, "efi_boot_entry: No entry specified");
    }

    /* Convert entry string to UTF-16 */
    CHAR16 boot_entry_utf16[128];
    size_t i;

    for (i = 0; i < sizeof(boot_entry_utf16) / sizeof(CHAR16) - 1 && boot_entry[i]; i++) {
        boot_entry_utf16[i] = (CHAR16)boot_entry[i];
    }

    boot_entry_utf16[i] = L'\0';

    /* Find the desired boot entry */
    uint16_t boot_next = 0;

    if (!find_boot_entry(boot_entry_utf16, &boot_next)) {
        panic(true, "efi_boot_entry: Failed to find boot entry '%s'", boot_entry);
    }

    EFI_GUID global_variable = EFI_GLOBAL_VARIABLE;

    /* Set BootNext to it */
    EFI_STATUS status = gRT->SetVariable(L"BootNext", &global_variable,
                                       EFI_VARIABLE_NON_VOLATILE |
                                       EFI_VARIABLE_BOOTSERVICE_ACCESS |
                                       EFI_VARIABLE_RUNTIME_ACCESS,
                                       sizeof(boot_next), &boot_next);

    if (status) {
        panic(true, "efi_boot_entry: Failed to set BootNext variable (%X)",
            (uint64_t)status);
    }

    /* Now reboot */
    gRT->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);

    panic(true, "efi_boot_entry: Failed to reboot");
}

#endif