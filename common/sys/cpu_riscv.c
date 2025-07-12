
#if defined(__riscv)

#include <lib/acpi.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <sys/cpu.h>
#include <mm/pmm.h>
#include <stddef.h>
#include <stdint.h>
#include <libfdt.h>

// ACPI RISC-V Hart Capabilities Table
struct rhct {
    struct sdt header;
    uint32_t flags;
    uint64_t time_base_frequency;
    uint32_t nodes_len;
    uint32_t nodes_offset;
    uint8_t nodes[];
} __attribute__((packed));

#define RHCT_ISA_STRING 0
#define RHCT_CMO        1
#define RHCT_MMU        2
#define RHCT_HART_INFO  65535

struct rhct_header {
    uint16_t type;      // node type
    uint16_t size;      // node size (bytes)
    uint16_t revision;  // node revision
} __attribute__((packed));

// One `struct rhct_hart_info` structure exists per hart in the system.
// The `offsets` array points to other entries in the RHCT associated with the
// hart.
struct rhct_hart_info {
    struct rhct_header header;
    uint16_t offsets_len;
    uint32_t acpi_processor_uid;
    uint32_t offsets[];
} __attribute__((packed));

struct rhct_isa_string {
    struct rhct_header header;
    uint16_t isa_string_len;
    const char isa_string[];
} __attribute__((packed));

#define RISCV_MMU_TYPE_SV39 0
#define RISCV_MMU_TYPE_SV48 1
#define RISCV_MMU_TYPE_SV57 2

struct rhct_mmu {
    struct rhct_header header;
    uint8_t reserved0;
    uint8_t mmu_type;
} __attribute__((packed));

size_t bsp_hartid;
struct riscv_hart *hart_list;
static struct riscv_hart *bsp_hart;

static struct riscv_hart *riscv_get_hart(size_t hartid) {
    for (struct riscv_hart *hart = hart_list; hart != NULL; hart = hart->next) {
        if (hart->hartid == hartid) {
            return hart;
        }
    }
    panic(false, "no `struct riscv_hart` for hartid %u", hartid);
}

static inline struct rhct_hart_info *rhct_get_hart_info(struct rhct *rhct, uint32_t acpi_uid) {
    uint32_t offset = rhct->nodes_offset;
    for (uint32_t i = 0; i < rhct->nodes_len; i++) {
        struct rhct_hart_info *node = (void *)((uintptr_t)rhct + offset);
        if (node->header.type == RHCT_HART_INFO && node->acpi_processor_uid == acpi_uid) {
            return node;
        }
        offset += node->header.size;
    }
    return NULL;
}

static void init_riscv_acpi(void) {
    struct madt *madt = acpi_get_table("APIC", 0);
    struct rhct *rhct = acpi_get_table("RHCT", 0);
    if (madt == NULL || rhct == NULL) {
        panic(false, "riscv: requires `APIC` and `RHCT` ACPI tables");
    }

    for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
         (uintptr_t)madt_ptr < (uintptr_t)madt + madt->header.length; madt_ptr += *(madt_ptr + 1)) {
        if (*madt_ptr != 0x18) {
            continue;
        }
        struct madt_riscv_intc *intc = (struct madt_riscv_intc *)madt_ptr;

        // Ignore harts we can't do anything with.
        if (!(intc->flags & MADT_RISCV_INTC_ENABLED ||
                intc->flags & MADT_RISCV_INTC_ONLINE_CAPABLE)) {
            continue;
        }

        uint32_t acpi_uid = intc->acpi_processor_uid;
        size_t hartid = intc->hartid;

        struct rhct_hart_info *hart_info = rhct_get_hart_info(rhct, acpi_uid);
        if (hart_info == NULL) {
            panic(false, "riscv: missing rhct node for hartid %u", hartid);
        }

        const char *isa_string = NULL;
        uint8_t mmu_type = 0;
        uint8_t flags = 0;

        for (uint32_t i = 0; i < hart_info->offsets_len; i++) {
            const struct rhct_header *node = (void *)((uintptr_t)rhct + hart_info->offsets[i]);
            switch (node->type) {
                case RHCT_ISA_STRING:
                    isa_string = ((struct rhct_isa_string *)node)->isa_string;
                    break;
                case RHCT_MMU:
                    mmu_type = ((struct rhct_mmu *)node)->mmu_type;
                    flags |= RISCV_HART_HAS_MMU;
                    break;
            }
        }

        if (isa_string == NULL) {
            print("riscv: missing isa string for hartid %u, skipping.\n", hartid);
            continue;
        }

        if (strncmp("rv64", isa_string, 4) && strncmp("rv32", isa_string, 4)) {
            print("riscv: skipping hartid %u with invalid isa string: %s", hartid, isa_string);
        }

        struct riscv_hart *hart = ext_mem_alloc(sizeof(struct riscv_hart));
        if (hart == NULL) {
            panic(false, "out of memory");
        }

        hart->hartid = hartid;
        hart->acpi_uid = acpi_uid;
        hart->isa_string = isa_string;
        hart->mmu_type = mmu_type;
        hart->flags = flags;

        hart->next = hart_list;
        hart_list = hart;

        if (hart->hartid == bsp_hartid) {
            bsp_hart = hart;
        }
    }
}

static void init_riscv_fdt(const void *fdt) {
    if (fdt_check_header(fdt)) {
        panic(false, "riscv: invalid device tree");
    }

    int cpus = fdt_path_offset(fdt, "/cpus");
    if (cpus < 0) {
        panic(false, "riscv: missing `/cpus` node");
    }

    int node;
    fdt_for_each_subnode(node, fdt, cpus) {
        const void *prop;

        if (!(prop = fdt_getprop(fdt, node, "device_type", NULL)) || strcmp(prop, "cpu")) {
            continue;
        }

        if (!(prop = fdt_getprop(fdt, node, "reg", NULL))) {
            continue;
        }
        size_t hartid = fdt32_ld(prop);

        uint8_t flags = 0;
        uint8_t mmu_type = 0;
        if ((prop = fdt_getprop(fdt, node, "mmu-type", NULL))) {
            if (!strcmp(prop, "riscv,sv39")) {
                mmu_type = RISCV_MMU_TYPE_SV39;
                flags |= RISCV_HART_HAS_MMU;
            } else if (!strcmp(prop, "riscv,sv48")) {
                mmu_type = RISCV_MMU_TYPE_SV48;
                flags |= RISCV_HART_HAS_MMU;
            } else if (!strcmp(prop, "riscv,sv57")) {
                mmu_type = RISCV_MMU_TYPE_SV57;
                flags |= RISCV_HART_HAS_MMU;
            }
        }

        const char *isa_string = fdt_getprop(fdt, node, "riscv,isa", NULL);
        if (isa_string == NULL) {
            print("riscv: missing isa string for hartid %u, skipping.\n", hartid);
            continue;
        }

        if (strncmp("rv64", isa_string, 4) && strncmp("rv32", isa_string, 4)) {
            print("riscv: skipping hartid %u with invalid isa string: %s", hartid, isa_string);
        }

        struct riscv_hart *hart = ext_mem_alloc(sizeof(struct riscv_hart));
        if (hart == NULL) {
            panic(false, "out of memory");
        }

        hart->hartid = hartid;
        hart->acpi_uid = 0;
        hart->isa_string = isa_string;
        hart->mmu_type = mmu_type;
        hart->flags = flags;

        hart->next = hart_list;
        hart_list = hart;

        if (hart->hartid == bsp_hartid) {
            bsp_hart = hart;
        }
    }
}

void init_riscv(const char *config) {
    void *fdt = get_device_tree_blob(config, 0);
    if (fdt != NULL) {
        init_riscv_fdt(fdt);
    } else if (acpi_get_rsdp()) {
        init_riscv_acpi();
    } else {
        panic(false, "riscv: requires DTB or ACPI");
    }

    if (bsp_hart == NULL) {
        panic(false, "riscv: missing `struct riscv_hart` for BSP");
    }

    if (strncasecmp(bsp_hart->isa_string, "rv64i", 5)) {
        panic(false, "unsupported cpu: %s", bsp_hart->isa_string);
    }

    for (struct riscv_hart *hart = hart_list; hart != NULL; hart = hart->next) {
        if (hart != bsp_hart && strcmp(bsp_hart->isa_string, hart->isa_string)) {
            hart->flags |= RISCV_HART_COPROC;
        }
    }
}

struct isa_extension {
    const char *name;
    size_t name_len;
    uint32_t ver_maj;
    uint32_t ver_min;
};

// Parse the next sequence of digit characters into an integer.
static bool parse_number(const char **s, size_t *_n) {
    size_t n = 0;
    bool parsed = false;
    while (isdigit(**s)) {
        n *= 10;
        n += *(*s)++ - '0';
        parsed = true;
    }
    *_n = n;
    return parsed;
}

// Parse the next extension from an ISA string.
static bool parse_extension(const char **s, struct isa_extension *ext) {
    if (**s == '\0') {
        return false;
    }

    const char *name = *s;
    size_t name_len = 1;
    if (**s == 's' || **s == 'S' || **s == 'x' || **s == 'X' || **s == 'z' || **s == 'Z') {
        while (isalpha((*s)[name_len])) {
            name_len++;
        }
    }
    *s += name_len;

    size_t maj = 0, min = 0;
    if (parse_number(s, &maj)) {
        if (**s == 'p') {
            *s += 1;
            parse_number(s, &min);
        }
    }

    while (**s == '_') {
        *s += 1;
    }

    if (ext) {
        ext->name = name;
        ext->name_len = name_len;
        ext->ver_maj = maj;
        ext->ver_min = min;
    }
    return true;
}

static bool extension_matches(const struct isa_extension *ext, const char *name) {
    for (size_t i = 0; i < ext->name_len; i++) {
        const char c1 = tolower(ext->name[i]);
        const char c2 = tolower(*name++);
        if (c2 == '\0' || c1 != c2) {
            return false;
        }
    }
    // Make sure `name` is not longer.
    return *name == '\0';
}

bool riscv_check_isa_extension_for(size_t hartid, const char *name, size_t *maj, size_t *min) {
    // Skip the `rv{32,64}` prefix so it's not parsed as extensions.
    const char *isa_string = riscv_get_hart(hartid)->isa_string + 4;

    struct isa_extension ext;
    while (parse_extension(&isa_string, &ext)) {
        if (!extension_matches(&ext, name)) {
            continue;
        }
        if (maj) {
            *maj = ext.ver_maj;
        }
        if (min) {
            *min = ext.ver_min;
        }
        return true;
    }

    return false;
}

#endif
