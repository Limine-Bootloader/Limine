#ifndef LIB__ACPI_H__
#define LIB__ACPI_H__

#include <stdint.h>
#include <stddef.h>
#include <sys/cpu.h>

#define EBDA (ebda_get())

#if defined (BIOS)
static inline uintptr_t ebda_get(void) {
    uintptr_t ebda = (uintptr_t)mminw(0x40e) << 4;

    // Sanity checks
    if (ebda < 0x80000 || ebda >= 0xa0000) {
        ebda = 0x80000;
    }

    return ebda;
}
#endif

struct sdt {
    char     signature[4];
    uint32_t length;
    uint8_t  rev;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_rev;
    char     creator_id[4];
    uint32_t creator_rev;
} __attribute__((packed));

struct rsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  rev;
    uint32_t rsdt_addr;
    // Revision 2 only after this comment
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct rsdt {
    struct sdt header;
    char ptrs_start[];
} __attribute__((packed));

struct smbios_entry_point_32 {
    char anchor_str[4];
    /// This value summed with all the values of the table.
    uint8_t checksum;
    /// Length of the entry point table.
    uint8_t length;
    /// Major version of SMBIOS.
    uint8_t major_version;
    /// Minor version of SMBIOS.
    uint8_t minor_version;
    /// Size of the largest SMBIOS structure, in bytes, and encompasses the
    /// structure’s formatted area and text strings
    uint16_t max_structure_size;
    uint8_t entry_point_revision;
    char formatted_area[5];

    char intermediate_anchor_str[5];
    /// Checksum for values from intermediate anchor str to the
    /// end of table.
    uint8_t intermediate_checksum;
    /// Total length of SMBIOS Structure Table, pointed to by the structure
    /// table address, in bytes.
    uint16_t table_length;
    /// 32-bit physical starting address of the read-only SMBIOS Structure
    /// Table.
    uint32_t table_address;
    /// Total number of structures present in the SMBIOS Structure Table.
    uint16_t number_of_structures;
    /// Indicates compliance with a revision of this specification.
    uint8_t bcd_revision;
} __attribute__((packed));

struct smbios_entry_point_64 {
    char anchor_str[5];
    /// This value summed with all the values of the table.
    uint8_t checksum;
    /// Length of the entry point table.
    uint8_t length;
    /// Major version of SMBIOS.
    uint8_t major_version;
    /// Minor version of SMBIOS.
    uint8_t minor_version;
    uint8_t docrev;
    uint8_t entry_point_revision;
    uint8_t reserved;
    /// Size of the SMBIOS Structure Table, in bytes.
    uint32_t table_maximum_size;
    /// 64-bit physical starting address of the read-only SMBIOS Structure
    /// Table.
    uint64_t table_address;
} __attribute__((packed));

struct madt {
    struct sdt header;
    uint32_t local_controller_addr;
    uint32_t flags;
    char     madt_entries_begin[];
} __attribute__((packed));

struct madt_header {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

struct madt_lapic {
    struct madt_header header;
    uint8_t  acpi_processor_uid;
    uint8_t  lapic_id;
    uint32_t flags;
} __attribute__((packed));

struct madt_x2apic {
    struct madt_header header;
    uint8_t  reserved[2];
    uint32_t x2apic_id;
    uint32_t flags;
    uint32_t acpi_processor_uid;
} __attribute__((packed));

#define MADT_LAPIC_ENABLED ((uint32_t)1 << 0)

struct madt_io_apic {
    uint8_t type;
    uint8_t length;
    uint8_t apic_id;
    uint8_t reserved;
    uint32_t address;
    uint32_t gsib;
} __attribute__((packed));

struct madt_lapic_nmi {
    struct madt_header header;   // type=4, length=6
    uint8_t  acpi_processor_uid; // 0xff = all processors
    uint16_t flags;              // MPS INTI flags
    uint8_t  lint;               // 0 or 1
} __attribute__((packed));

struct madt_x2apic_nmi {
    struct madt_header header;   // type=0x0a, length=12
    uint16_t flags;              // MPS INTI flags
    uint32_t acpi_processor_uid; // 0xffffffff = all processors
    uint8_t  lint;               // 0 or 1
    uint8_t  reserved[3];
} __attribute__((packed));

struct madt_gicc {
    struct madt_header header;
    uint8_t  reserved1[2];
    uint32_t iface_no;
    uint32_t acpi_uid;
    uint32_t flags;
    uint32_t parking_ver;
    uint32_t perf_gsiv;
    uint64_t parking_addr;
    uint64_t gicc_base_addr;
    uint64_t gicv_base_addr;
    uint64_t gich_base_addr;
    uint32_t vgic_maint_gsiv;
    uint64_t gicr_base_addr;
    uint64_t mpidr;
    uint8_t  power_eff_class;
    uint8_t  reserved2;
    uint16_t spe_overflow_gsiv;
} __attribute__((packed));

// Reference: https://github.com/riscv-non-isa/riscv-acpi/issues/15
struct madt_riscv_intc {
    struct madt_header header;
    uint8_t version;
    uint8_t reserved;
    uint32_t flags;
    uint64_t hartid;
    uint32_t acpi_processor_uid;
} __attribute__((packed));

#define MADT_RISCV_INTC_ENABLED        ((uint32_t)1 << 0)
#define MADT_RISCV_INTC_ONLINE_CAPABLE ((uint32_t)1 << 1)

struct madt_core_pic {
    struct madt_header header;
    uint8_t  version;
    uint32_t acpi_processor_uid;
    uint32_t core_id;
    uint32_t flags;
} __attribute__((packed));

#define MADT_CORE_PIC_ENABLED        ((uint32_t)1 << 0)
#define MADT_CORE_PIC_ONLINE_CAPABLE ((uint32_t)1 << 1)

uint8_t acpi_checksum(void *ptr, size_t size);
void   *acpi_get_rsdp(void);

void   *acpi_get_rsdp_v1(void);
void   *acpi_get_rsdp_v2(void);

void   *acpi_get_table(const char *signature, int index);
void    acpi_get_smbios(void **smbios32, void **smbios64);

void acpi_map_tables(void);
void smbios_map_tables(void);

#if defined (UEFI)
void efi_map_runtime_entries(void);
#endif

#endif
