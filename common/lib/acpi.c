#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <lib/acpi.h>
#include <lib/misc.h>
#include <lib/libc.h>
#include <lib/print.h>
#include <mm/pmm.h>

// Following function based on https://github.com/managarm/lai/blob/master/helpers/pc-bios.c's function lai_bios_calc_checksum()
uint8_t acpi_checksum(void *ptr, size_t size) {
    uint8_t sum = 0, *_ptr = ptr;
    for (size_t i = 0; i < size; i++)
        sum += _ptr[i];
    return sum;
}

#if defined (BIOS)

void *acpi_get_rsdp(void) {
    size_t ebda = EBDA;

    for (size_t i = ebda; i < 0x100000; i += 16) {
        if (i == ebda + 1024) {
            // We probed the 1st KiB of the EBDA as per spec, move onto 0xe0000
            i = 0xe0000;
        }
        if (!memcmp((char *)i, "RSD PTR ", 8)
         && !acpi_checksum((void *)i, 20)) {
            printv("acpi: Found RSDP at %p\n", i);
            return (void *)i;
        }
    }

    return NULL;
}

/// Returns the RSDP v1 pointer if available or else NULL.
void *acpi_get_rsdp_v1(void) {
    // In BIOS according to the ACPI spec (see ACPI 6.2 section
    // 5.2.5.1 'Finding the RSDP on IA-PC Systems') it either contains
    // the RSDP or the XSDP and it cannot contain both. So, we directly
    // use acpi_get_rsdp function to find the RSDP and if it has the correct
    // revision, return it.
    struct rsdp *rsdp = acpi_get_rsdp();

    if (rsdp != NULL && rsdp->rev < 2)
        return rsdp;

    return NULL;
}

void acpi_get_smbios(void **smbios32, void **smbios64) {
    *smbios32 = NULL;
    *smbios64 = NULL;

    for (size_t i = 0xf0000; i < 0x100000; i += 16) {
        struct smbios_entry_point_32 *ptr = (struct smbios_entry_point_32 *)i;

        if (!memcmp(ptr->anchor_str, "_SM_", 4) &&
            ptr->length >= sizeof(struct smbios_entry_point_32) &&
            !acpi_checksum((void *)ptr, ptr->length)) {
            printv("acpi: Found SMBIOS 32-bit entry point at %p\n", i);
            *smbios32 = (void *)ptr;
            break;
        }
    }

    for (size_t i = 0xf0000; i < 0x100000; i += 16) {
        struct smbios_entry_point_64 *ptr = (struct smbios_entry_point_64 *)i;

        if (!memcmp(ptr->anchor_str, "_SM3_", 5) &&
            ptr->length >= sizeof(struct smbios_entry_point_64) &&
            !acpi_checksum((void *)ptr, ptr->length)) {
            printv("acpi: Found SMBIOS 64-bit entry point at %p\n", i);
            *smbios64 = (void *)ptr;
            break;
        }
    }
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

/// Returns the RSDP v1 pointer if available or else NULL.
void *acpi_get_rsdp_v1(void) {
    // To maintain GRUB compatibility we will need to probe for the RSDP
    // again since UEFI can contain both XSDP and RSDP (see ACPI 6.2 section
    // 5.2.5.2 'Finding the RSDP on UEFI Enabled Systems') and in the acpi_get_rsdp
    // function we look for the RSDP with the latest revision.
    EFI_GUID acpi_1_guid = ACPI_TABLE_GUID;

    for (size_t i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *cur_table = &gST->ConfigurationTable[i];

        if (memcmp(&cur_table->VendorGuid, &acpi_1_guid, sizeof(EFI_GUID)) != 0)
            continue;

        if (acpi_checksum(cur_table->VendorTable, 20) != 0)
            continue;

        return (void *)cur_table->VendorTable;
    }

    return NULL;
}

void acpi_get_smbios(void **smbios32, void **smbios64) {
    *smbios32 = NULL;
    *smbios64 = NULL;

    for (size_t i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *cur_table = &gST->ConfigurationTable[i];
        EFI_GUID smbios_guid = SMBIOS_TABLE_GUID;

        if (memcmp(&cur_table->VendorGuid, &smbios_guid, sizeof(EFI_GUID)) != 0)
            continue;

        struct smbios_entry_point_32 *ptr = (struct smbios_entry_point_32 *)cur_table->VendorTable;

        if (ptr->length < sizeof(struct smbios_entry_point_32)
         || acpi_checksum((void *)ptr, ptr->length) != 0)
            continue;

        printv("acpi: Found SMBIOS 32-bit entry point at %p\n", ptr);

        *smbios32 = (void *)ptr;

        break;
    }

    for (size_t i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *cur_table = &gST->ConfigurationTable[i];
        EFI_GUID smbios3_guid = SMBIOS3_TABLE_GUID;

        if (memcmp(&cur_table->VendorGuid, &smbios3_guid, sizeof(EFI_GUID)) != 0)
            continue;

        struct smbios_entry_point_64 *ptr = (struct smbios_entry_point_64 *)cur_table->VendorTable;

        if (ptr->length < sizeof(struct smbios_entry_point_64)
         || acpi_checksum((void *)ptr, ptr->length) != 0)
            continue;

        printv("acpi: Found SMBIOS 64-bit entry point at %p\n", ptr);

        *smbios64 = (void *)ptr;

        break;
    }
}

#endif

/// Returns the RSDP v2 pointer if available or else NULL.
void *acpi_get_rsdp_v2(void) {
    // Since the acpi_get_rsdp function already looks for the XSDP we can
    // just check if it has the correct revision and return the pointer :^)
    struct rsdp *rsdp = acpi_get_rsdp();

    if (rsdp != NULL && rsdp->rev >= 2)
        return rsdp;

    return NULL;
}

void *acpi_get_table(const char *signature, int index) {
    int cnt = 0;

    struct rsdp *rsdp = acpi_get_rsdp();
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
        printv("acpi: Invalid %s header length\n", use_xsdt ? "XSDT" : "RSDT");
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
                printv("acpi: warning: bad checksum in \"%s\", using anyway\n", signature);
            }
            printv("acpi: Found \"%s\" at %p\n", signature, ptr);
            return ptr;
        }
    }

    printv("acpi: \"%s\" not found\n", signature);
    return NULL;
}

static bool acpi_padding_is_safe(uint64_t base, uint64_t length) {
    if (length == 0) {
        return true;
    }

    uint64_t top = CHECKED_ADD(base, length, return false);

    for (size_t i = 0; i < memmap_entries; i++) {
        uint64_t entry_base = memmap[i].base;
        uint64_t entry_top  = CHECKED_ADD(entry_base, memmap[i].length, continue);

        if (entry_base >= top || entry_top <= base) {
            continue;
        }

        if (memmap[i].type != MEMMAP_USABLE && memmap[i].type != MEMMAP_RESERVED) {
            return false;
        }
    }

    return true;
}

static void map_single_table(uint64_t addr, uint32_t len) {
#if defined (__i386__)
    if (addr >= 0x100000000) {
        print("acpi: warning: Cannot get length of ACPI table above 4GiB\n");
        return;
    }
#endif

    uint32_t length = len != (uint32_t)-1 ? len : *(uint32_t *)(uintptr_t)(addr + 4);

    uint64_t aligned_base = ALIGN_DOWN(addr, 4096);
    uint64_t aligned_top  = ALIGN_UP(addr + length, 4096, panic(false, "acpi: Alignment overflow"));

    if (!acpi_padding_is_safe(aligned_base, addr - aligned_base)) {
        aligned_base = addr;
    }
    if (!acpi_padding_is_safe(addr + length, aligned_top - (addr + length))) {
        aligned_top = addr + length;
    }

    uint64_t memmap_type = pmm_check_type(addr);

    if (memmap_type != MEMMAP_ACPI_RECLAIMABLE && memmap_type != MEMMAP_ACPI_NVS) {
        memmap_alloc_range(aligned_base, aligned_top - aligned_base, MEMMAP_RESERVED_MAPPED, 0, true, false, true);
    }
}


void acpi_map_tables(void) {
    struct rsdp *rsdp = acpi_get_rsdp();
    if (rsdp == NULL)
        return;

    uint64_t rsdp_length;
    if (rsdp->rev < 2) {
        rsdp_length = 20;
    } else {
        rsdp_length = rsdp->length;
    }

    map_single_table((uintptr_t)rsdp, rsdp_length);

    if (!(rsdp->rev >= 2 && rsdp->xsdt_addr)) {
        goto no_xsdt;
    }

    struct rsdt *xsdt = (void *)(uintptr_t)rsdp->xsdt_addr;
    if (xsdt->header.length < sizeof(struct sdt)) {
        goto no_xsdt;
    }
    size_t xsdt_entry_count = (xsdt->header.length - sizeof(struct sdt)) / 8;

    map_single_table((uintptr_t)xsdt, (uint32_t)-1);

    for (size_t i = 0; i < xsdt_entry_count; i++) {
        uint64_t entry = ((uint64_t *)xsdt->ptrs_start)[i];
        if (entry == 0)
            continue;
        struct sdt *sdt = (void *)(uintptr_t)entry;

        map_single_table((uintptr_t)sdt, (uint32_t)-1);
    }

no_xsdt:;
    if (rsdp->rsdt_addr == 0) {
        goto no_rsdt;
    }

    struct rsdt *rsdt = (void *)(uintptr_t)rsdp->rsdt_addr;
    if (rsdt->header.length < sizeof(struct sdt)) {
        goto no_rsdt;
    }
    size_t rsdt_entry_count = (rsdt->header.length - sizeof(struct sdt)) / 4;

    map_single_table((uintptr_t)rsdt, (uint32_t)-1);

    for (size_t i = 0; i < rsdt_entry_count; i++) {
        uint32_t entry = ((uint32_t *)rsdt->ptrs_start)[i];
        if (entry == 0)
            continue;
        struct sdt *sdt = (void *)(uintptr_t)entry;

        map_single_table((uintptr_t)sdt, (uint32_t)-1);
    }

no_rsdt:;
    uint8_t *fadt = acpi_get_table("FACP", 0);
    if (fadt == NULL) {
        return;
    }
    uint32_t fadt_length;
    memcpy(&fadt_length, fadt + 4, sizeof(fadt_length));

    // Read the single fields from the FADT without defining a struct for the whole table
    if (fadt_length >= 132 + 8) {
        uint64_t x_facs;
        memcpy(&x_facs, fadt + 132, sizeof(x_facs));
        if (x_facs != 0) {
            map_single_table(x_facs, (uint32_t)-1);
        }
    }
    if (fadt_length >= 140 + 8) {
        uint64_t x_dsdt;
        memcpy(&x_dsdt, fadt + 140, sizeof(x_dsdt));
        if (x_dsdt != 0) {
            map_single_table(x_dsdt, (uint32_t)-1);
        }
    }
    if (fadt_length >= 36 + 4) {
        uint32_t facs;
        memcpy(&facs, fadt + 36, sizeof(facs));
        if (facs != 0) {
            map_single_table(facs, (uint32_t)-1);
        }
    }
    if (fadt_length >= 40 + 4) {
        uint32_t dsdt;
        memcpy(&dsdt, fadt + 40, sizeof(dsdt));
        if (dsdt != 0) {
            map_single_table(dsdt, (uint32_t)-1);
        }
    }
}

void smbios_map_tables(void) {
    void *smbios32_ptr = NULL, *smbios64_ptr = NULL;
    acpi_get_smbios(&smbios32_ptr, &smbios64_ptr);

    if (smbios32_ptr != NULL) {
        struct smbios_entry_point_32 *smbios32 = smbios32_ptr;
        map_single_table((uintptr_t)smbios32, smbios32->length);
        if (smbios32->table_address != 0) {
            map_single_table(smbios32->table_address, smbios32->table_length);
        }
    }

    if (smbios64_ptr != NULL) {
        struct smbios_entry_point_64 *smbios64 = smbios64_ptr;
        map_single_table((uintptr_t)smbios64, smbios64->length);
        if (smbios64->table_address != 0) {
            map_single_table(smbios64->table_address, smbios64->table_maximum_size);
        }
    }
}

#if defined (UEFI)
void efi_map_runtime_entries(void) {
    size_t entry_count = efi_mmap_size / efi_desc_size;

    for (size_t i = 0; i < entry_count; i++) {
        EFI_MEMORY_DESCRIPTOR *entry = (void *)efi_mmap + i * efi_desc_size;

        if (entry->Type != EfiRuntimeServicesCode
         && entry->Type != EfiRuntimeServicesData) {
            continue;
        }

        uint64_t base = entry->PhysicalStart;
        uint64_t length = CHECKED_MUL(entry->NumberOfPages, 4096, continue);

        memmap_alloc_range(base, length, MEMMAP_RESERVED_MAPPED, 0, true, false, true);
    }

    // Explicitly map the EFI system table and the data it references.
    // The UEFI spec does not guarantee these reside in EfiRuntimeServicesData,
    // so we map them separately to ensure they are always accessible via HHDM.
    map_single_table((uintptr_t)gST, sizeof(*gST));

    if (gST->RuntimeServices != NULL) {
        map_single_table((uintptr_t)gST->RuntimeServices,
                         sizeof(*gST->RuntimeServices));
    }

    if (gST->ConfigurationTable != NULL && gST->NumberOfTableEntries > 0) {
        uint64_t ct_size = CHECKED_MUL(gST->NumberOfTableEntries,
                (uint64_t)sizeof(EFI_CONFIGURATION_TABLE), goto skip_ct);
        if (ct_size <= UINT32_MAX) {
            map_single_table((uintptr_t)gST->ConfigurationTable, (uint32_t)ct_size);
        }
skip_ct:;
    }

    if (gST->FirmwareVendor != NULL) {
        size_t len = 0;
        while (gST->FirmwareVendor[len] != 0) {
            len++;
        }
        map_single_table((uintptr_t)gST->FirmwareVendor,
                         (len + 1) * sizeof(*gST->FirmwareVendor));
    }
}
#endif
