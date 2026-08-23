#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <lib/acpi.h>
#include <lib/misc.h>
#include <lib/libc.h>
#include <lib/print.h>

// Following function based on https://github.com/managarm/lai/blob/master/helpers/pc-bios.c's function lai_bios_calc_checksum()
uint8_t acpi_checksum(void *ptr, size_t size) {
    uint8_t sum = 0, *_ptr = ptr;
    for (size_t i = 0; i < size; i++)
        sum += _ptr[i];
    return sum;
}

#if defined (BIOS)

static void *acpi_get_rsdp_impl(bool log) {
    size_t ebda = EBDA;

    for (size_t i = ebda; i < 0x100000; i += 16) {
        if (i == ebda + 1024) {
            // We probed the 1st KiB of the EBDA as per spec, move onto 0xe0000
            i = 0xe0000;
        }
        if (!memcmp((char *)i, "RSD PTR ", 8)
         && !acpi_checksum((void *)i, 20)) {
            if (log) {
                printv("acpi: Found RSDP at %p\n", i);
            }
            return (void *)i;
        }
    }

    return NULL;
}

void *acpi_get_rsdp(void) {
    return acpi_get_rsdp_impl(true);
}

#endif

#if defined (UEFI)

#include <efi.h>

void *acpi_get_rsdp(void) {
    EFI_GUID acpi_2_guid = ACPI_20_TABLE_GUID;
    EFI_GUID acpi_1_guid = ACPI_TABLE_GUID;

    void *rsdp = NULL;

    for (size_t i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *cur_table = &gST->ConfigurationTable[i];

        bool is_xsdp = memcmp(&cur_table->VendorGuid, &acpi_2_guid, sizeof(EFI_GUID)) == 0;
        bool is_rsdp = memcmp(&cur_table->VendorGuid, &acpi_1_guid, sizeof(EFI_GUID)) == 0;

        if (!is_xsdp && !is_rsdp)
            continue;

        if ((is_xsdp && acpi_checksum(cur_table->VendorTable, sizeof(struct rsdp)) != 0) || // XSDP is 36 bytes wide
            (is_rsdp && acpi_checksum(cur_table->VendorTable, 20) != 0)) // RSDP is 20 bytes wide
            continue;

        printv("acpi: Found %s at %p\n", is_xsdp ? "XSDP" : "RSDP", cur_table->VendorTable);

        // We want to return the XSDP if it exists rather then returning
        // the RSDP. We need to add a check for that since the table entries
        // are not in the same order for all EFI systems since it might be the
        // case where the RSDP occurs before the XSDP.
        if (is_xsdp) {
            rsdp = (void *)cur_table->VendorTable;
            break; // Found it!.
        } else {
            // Found the RSDP but we continue to loop since we might
            // find the XSDP.
            rsdp = (void *)cur_table->VendorTable;
        }
    }

    return rsdp;
}

#endif

// A false log is not cosmetic: serial.c reaches this from inside serial_out(),
// so anything below that prints unconditionally would recurse through it.
static void *acpi_get_table_impl(const char *signature, int index, bool log) {
    int cnt = 0;

#if defined (BIOS)
    struct rsdp *rsdp = acpi_get_rsdp_impl(log);
#else
    struct rsdp *rsdp = acpi_get_rsdp();
#endif
    if (rsdp == NULL)
        return NULL;

    bool use_xsdt = false;
    if (rsdp->rev >= 2 && rsdp->xsdt_addr
     && (sizeof(uintptr_t) >= 8 || rsdp->xsdt_addr <= UINT32_MAX))
        use_xsdt = true;

    struct rsdt *rsdt;
    if (use_xsdt)
        rsdt = (struct rsdt *)(uintptr_t)rsdp->xsdt_addr;
    else
        rsdt = (struct rsdt *)(uintptr_t)rsdp->rsdt_addr;

    if (rsdt == NULL) {
        return NULL;
    }

    // Validate RSDT/XSDT header length
    if (rsdt->header.length < sizeof(struct sdt)) {
        if (log) {
            printv("acpi: Invalid %s header length\n", use_xsdt ? "XSDT" : "RSDT");
        }
        return NULL;
    }

    size_t entry_size = use_xsdt ? 8 : 4;
    size_t entry_count = (rsdt->header.length - sizeof(struct sdt)) / entry_size;

    for (size_t i = 0; i < entry_count; i++) {
        struct sdt *ptr;
        if (use_xsdt)
            ptr = (struct sdt *)(uintptr_t)((uint64_t *)rsdt->ptrs_start)[i];
        else
            ptr = (struct sdt *)(uintptr_t)((uint32_t *)rsdt->ptrs_start)[i];

        if (ptr == NULL) {
            continue;
        }

        if (!memcmp(ptr->signature, signature, 4)
         && cnt++ == index) {
            if (acpi_checksum(ptr, ptr->length)) {
                if (log) {
                    printv("acpi: warning: bad checksum in \"%s\", using anyway\n", signature);
                }
            }
            if (log) {
                printv("acpi: Found \"%s\" at %p\n", signature, ptr);
            }
            return ptr;
        }
    }

    if (log) {
        printv("acpi: \"%s\" not found\n", signature);
    }
    return NULL;
}

void *acpi_get_table(const char *signature, int index) {
    return acpi_get_table_impl(signature, index, true);
}

#if defined (BIOS)
void *acpi_get_table_quiet(const char *signature, int index) {
    return acpi_get_table_impl(signature, index, false);
}
#endif
