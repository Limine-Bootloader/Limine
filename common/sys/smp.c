#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <lib/libc.h>
#include <lib/acpi.h>
#include <sys/cpu.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <sys/smp.h>
#include <sys/lapic.h>
#include <sys/gdt.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/mtrr.h>
#define LIMINE_NO_POINTERS
#include <limine.h>
#if defined (__riscv)
#include <sys/sbi.h>
#endif
#if defined (__aarch64__)
#include <libfdt.h>
#endif

extern symbol smp_trampoline_start;
extern size_t smp_trampoline_size;

#if defined (__x86_64__) || defined (__i386__)

struct trampoline_passed_info {
    uint8_t  smp_tpl_booted_flag;
    uint8_t  smp_tpl_target_mode;
    uint32_t smp_tpl_pagemap;
    uint32_t smp_tpl_info_struct;
    struct gdtr smp_tpl_gdt;
    uint64_t smp_tpl_hhdm;
    uint64_t smp_tpl_bsp_apic_addr_msr;
    uint64_t smp_tpl_mtrr_restore;
    uint64_t smp_tpl_temp_stack;
} __attribute__((packed));

static bool smp_start_ap(uint32_t lapic_id, struct gdtr *gdtr,
                         struct limine_mp_info *info_struct,
                         int paging_mode, uint32_t pagemap,
                         bool x2apic, bool nx, uint64_t hhdm, bool wp) {
    // Prepare the trampoline
    static void *trampoline = NULL;
    if (trampoline == NULL) {
        trampoline = conv_mem_alloc(smp_trampoline_size);

        memcpy(trampoline, smp_trampoline_start, smp_trampoline_size);
    }

    static void *temp_stack = NULL;
    if (temp_stack == NULL) {
        temp_stack = ext_mem_alloc(8192);
    }

    static struct trampoline_passed_info *passed_info = NULL;
    if (passed_info == NULL) {
        passed_info = (void *)(((uintptr_t)trampoline + smp_trampoline_size)
                               - sizeof(struct trampoline_passed_info));
    }

    passed_info->smp_tpl_info_struct = (uint32_t)(uintptr_t)info_struct;
    passed_info->smp_tpl_booted_flag = 0;
    passed_info->smp_tpl_pagemap     = pagemap;
    passed_info->smp_tpl_target_mode = ((uint32_t)(paging_mode == PAGING_MODE_X86_64_5LVL) << 1)
                                     | ((uint32_t)nx << 3)
                                     | ((uint32_t)wp << 4);
    passed_info->smp_tpl_gdt = *gdtr;
    passed_info->smp_tpl_hhdm = hhdm;
    passed_info->smp_tpl_bsp_apic_addr_msr = rdmsr(0x1b);
    passed_info->smp_tpl_mtrr_restore = (uint64_t)(uintptr_t)mtrr_restore;
    passed_info->smp_tpl_temp_stack = (uint64_t)(uintptr_t)temp_stack;

    asm volatile ("" ::: "memory");

    // Send the INIT IPI
    if (x2apic) {
        x2apic_write(LAPIC_REG_ICR0, ((uint64_t)lapic_id << 32) | 0x4500);
    } else {
        lapic_write(LAPIC_REG_ICR1, lapic_id << 24);
        lapic_write(LAPIC_REG_ICR0, 0x4500);
    }
    delay(10000000);

    // Send the Startup IPI
    if (x2apic) {
        x2apic_write(LAPIC_REG_ICR0, ((uint64_t)lapic_id << 32) |
                                     ((size_t)trampoline / 4096) | 0x4600);
    } else {
        lapic_write(LAPIC_REG_ICR1, lapic_id << 24);
        lapic_write(LAPIC_REG_ICR0, ((size_t)trampoline / 4096) | 0x4600);
    }

    for (int i = 0; i < 100; i++) {
        if (locked_read(&passed_info->smp_tpl_booted_flag) == 1) {
            return true;
        }
        delay(10000000);
    }

    return false;
}

struct limine_mp_info *init_smp(size_t   *cpu_count,
                                 uint32_t *_bsp_lapic_id,
                                 int       paging_mode,
                                 pagemap_t pagemap,
                                 bool      x2apic,
                                 bool      nx,
                                 uint64_t  hhdm,
                                 bool      wp) {
    if (!lapic_check())
        return NULL;

    // Search for MADT table
    struct madt *madt = acpi_get_table("APIC", 0);

    if (madt == NULL)
        return NULL;

    struct gdtr gdtr = gdt;

    uint8_t bsp_lapic_id;
    uint32_t bsp_x2apic_id;

    // If x2APIC already enabled by BIOS, then xAPIC is not available
    if (rdmsr(0x1b) & (1 << 10)) {
        if (!x2apic) {
            panic(false, "smp: Kernel does not support x2APIC, but machine requires it");
        }
    }

    x2apic = x2apic && x2apic_enable();

    if (x2apic) {
        bsp_x2apic_id = x2apic_read(LAPIC_REG_ID);
        bsp_lapic_id = bsp_x2apic_id;
    } else {
        bsp_lapic_id = lapic_read(LAPIC_REG_ID) >> 24;
        bsp_x2apic_id = bsp_lapic_id;
    }

    *_bsp_lapic_id = bsp_x2apic_id;

    *cpu_count = 0;

    // Count the MAX of startable APs and allocate accordingly
    size_t max_cpus = 0;

    for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
      (uintptr_t)madt_ptr < (uintptr_t)madt + madt->header.length;
      madt_ptr += *(madt_ptr + 1)) {
        switch (*madt_ptr) {
            case 0: {
                // Processor local xAPIC
                struct madt_lapic *lapic = (void *)madt_ptr;

                // Check if we can actually try to start the AP
                if ((lapic->flags & 1) ^ ((lapic->flags >> 1) & 1))
                    max_cpus++;

                continue;
            }
            case 9: {
                // Processor local x2APIC
                if (!x2apic)
                    continue;

                struct madt_x2apic *x2lapic = (void *)madt_ptr;

                // Check if we can actually try to start the AP
                if ((x2lapic->flags & 1) ^ ((x2lapic->flags >> 1) & 1))
                    max_cpus++;

                continue;
            }
        }
    }

    if (max_cpus == 0) {
        return NULL;
    }

    struct limine_mp_info *ret = ext_mem_alloc(max_cpus * sizeof(struct limine_mp_info));
    *cpu_count = 0;

    // Try to start all APs
    mtrr_save();

    for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
      (uintptr_t)madt_ptr < (uintptr_t)madt + madt->header.length;
      madt_ptr += *(madt_ptr + 1)) {
        switch (*madt_ptr) {
            case 0: {
                // Processor local xAPIC
                struct madt_lapic *lapic = (void *)madt_ptr;

                // Check if we can actually try to start the AP
                if (!((lapic->flags & 1) ^ ((lapic->flags >> 1) & 1)))
                    continue;

                struct limine_mp_info *info_struct = &ret[*cpu_count];

                info_struct->processor_id = lapic->acpi_processor_uid;
                info_struct->lapic_id = lapic->lapic_id;

                // Do not try to restart the BSP
                if (lapic->lapic_id == bsp_lapic_id) {
                    (*cpu_count)++;
                    continue;
                }

                printv("smp: [xAPIC] Found candidate AP for bring-up. LAPIC ID: %u\n", lapic->lapic_id);

                // Try to start the AP
                if (!smp_start_ap(lapic->lapic_id, &gdtr, info_struct,
                                  paging_mode, (uintptr_t)pagemap.top_level,
                                  x2apic, nx, hhdm, wp)) {
                    print("smp: FAILED to bring-up AP\n");
                    continue;
                }

                printv("smp: Successfully brought up AP\n");

                (*cpu_count)++;
                continue;
            }
            case 9: {
                // Processor local x2APIC
                if (!x2apic)
                    continue;

                struct madt_x2apic *x2lapic = (void *)madt_ptr;

                // Check if we can actually try to start the AP
                if (!((x2lapic->flags & 1) ^ ((x2lapic->flags >> 1) & 1)))
                    continue;

                struct limine_mp_info *info_struct = &ret[*cpu_count];

                info_struct->processor_id = x2lapic->acpi_processor_uid;
                info_struct->lapic_id = x2lapic->x2apic_id;

                // Do not try to restart the BSP
                if (x2lapic->x2apic_id == bsp_x2apic_id) {
                    (*cpu_count)++;
                    continue;
                }

                printv("smp: [x2APIC] Found candidate AP for bring-up. LAPIC ID: %u\n", x2lapic->x2apic_id);

                // Try to start the AP
                if (!smp_start_ap(x2lapic->x2apic_id, &gdtr, info_struct,
                                  paging_mode, (uintptr_t)pagemap.top_level,
                                  true, nx, hhdm, wp)) {
                    print("smp: FAILED to bring-up AP\n");
                    continue;
                }

                printv("smp: Successfully brought up AP\n");

                (*cpu_count)++;
                continue;
            }
        }
    }

    if (*cpu_count == 0) {
        pmm_free(ret, max_cpus * sizeof(struct limine_mp_info));
        return NULL;
    }

    return ret;
}

#elif defined (__aarch64__)

struct trampoline_passed_info {
    uint64_t smp_tpl_booted_flag;

    uint64_t smp_tpl_hhdm_offset;

    uint64_t smp_tpl_ttbr0;
    uint64_t smp_tpl_ttbr1;

    uint64_t smp_tpl_mair;
    uint64_t smp_tpl_tcr;
    uint64_t smp_tpl_sctlr;

    uint64_t smp_tpl_info_struct;
};

enum {
    BOOT_WITH_SPIN_TBL,
    BOOT_WITH_PSCI_SMC,
    BOOT_WITH_PSCI_HVC,
    BOOT_WITH_ACPI_PARK
};

static uint32_t psci_cpu_on = 0xC4000003;

static bool try_start_ap(int boot_method, uint64_t method_ptr,
                         struct limine_mp_info *info_struct,
                         uint64_t ttbr0, uint64_t ttbr1, uint64_t mair,
                         uint64_t tcr, uint64_t sctlr,
                         uint64_t hhdm_offset) {
    // Prepare the trampoline
    static void *trampoline = NULL;
    if (trampoline == NULL) {
        trampoline = ext_mem_alloc(smp_trampoline_size);

        memcpy(trampoline, smp_trampoline_start, smp_trampoline_size);
    }

    static struct trampoline_passed_info *passed_info = NULL;
    if (passed_info == NULL) {
        passed_info = (void *)(((uintptr_t)trampoline + 0x1000)
                               - sizeof(struct trampoline_passed_info));
    }

    passed_info->smp_tpl_info_struct = (uint64_t)(uintptr_t)info_struct;
    passed_info->smp_tpl_booted_flag = 0;
    passed_info->smp_tpl_ttbr0       = ttbr0;
    passed_info->smp_tpl_ttbr1       = ttbr1;
    passed_info->smp_tpl_mair        = mair;
    passed_info->smp_tpl_tcr         = tcr;
    passed_info->smp_tpl_sctlr       = sctlr;
    passed_info->smp_tpl_hhdm_offset = hhdm_offset;

    // Cache coherency between the I-Cache and D-Cache is not guaranteed by the
    // architecture and as such we must perform I-Cache invalidation.
    // Additionally, the newly-booted AP may have caches disabled which implies
    // it possibly does not see our cache contents either.

    clean_dcache_poc((uintptr_t)trampoline, (uintptr_t)trampoline + 0x1000);
    inval_icache_pou((uintptr_t)trampoline, (uintptr_t)trampoline + 0x1000);

    asm volatile ("" ::: "memory");

    switch (boot_method) {
        case BOOT_WITH_SPIN_TBL:
            *(volatile uint64_t *)method_ptr = (uint64_t)(uintptr_t)trampoline;
            clean_dcache_poc(method_ptr, method_ptr + 8);
            asm ("sev");
            break;

        case BOOT_WITH_PSCI_SMC:
        case BOOT_WITH_PSCI_HVC: {
            register int32_t result asm("w0");
            register uint32_t cmd asm("w0") = psci_cpu_on;
            register uint64_t cpu asm("x1") = info_struct->mpidr;
            register uint64_t addr asm("x2") = (uint64_t)(uintptr_t)trampoline;
            register uint64_t ctx asm("x3") = 0;

            if (boot_method == BOOT_WITH_PSCI_SMC)
                asm volatile ("smc #0" : "=r"(result) : "r"(cmd), "r"(cpu), "r"(addr), "r"(ctx));
            else
                asm volatile ("hvc #0" : "=r"(result) : "r"(cmd), "r"(cpu), "r"(addr), "r"(ctx));

            switch (result) {
                case 0: // Success
                    break;
                case -2:
                    printv("smp: PSCI says CPU_ON was given invalid arguments\n");
                    return false;
                case -4:
                    printv("smp: PSCI says AP is already on\n");
                    return false;
                case -5:
                    printv("smp: PSCI says CPU_ON is already pending for this AP\n");
                    return false;
                case -6:
                    printv("smp: PSCI reports internal failure\n");
                    return false;
                case -9:
                    printv("smp: PSCI says CPU_ON was given an invalid address\n");
                    return false;
                default:
                    printv("smp: PSCI reports an unexpected error (%d)\n", result);
                    return false;
            }

            break;
        }

        case BOOT_WITH_ACPI_PARK:
            panic(false, "ACPI parking protocol is unsupported, please report this!");
            break;

        default:
            panic(false, "Invalid boot method specified");
    }

    for (int i = 0; i < 1000000; i++) {
        // We do not need cache invalidation here as by the time the AP gets to
        // set this flag, it has enabled its caches

        if (locked_read(&passed_info->smp_tpl_booted_flag) == 1) {
            return true;
        }
        delay(100000);
    }

    return false;
}

static struct limine_mp_info *try_acpi_smp(size_t   *cpu_count,
                                            uint64_t *_bsp_mpidr,
                                            pagemap_t pagemap,
                                            uint64_t  mair,
                                            uint64_t  tcr,
                                            uint64_t  sctlr,
                                            uint64_t  hhdm_offset) {
    int boot_method = BOOT_WITH_ACPI_PARK;

    // Search for FADT table
    uint8_t *fadt = acpi_get_table("FACP", 0);

    if (fadt == NULL)
        return NULL;

    // Read the single field from the FADT without defining a struct for the whole table
    uint16_t arm_boot_args;
    memcpy(&arm_boot_args, fadt + 129, 2);

    if (arm_boot_args & 1) // PSCI compliant?
        boot_method = arm_boot_args & 2 ? BOOT_WITH_PSCI_HVC : BOOT_WITH_PSCI_SMC;

    // Search for MADT table
    struct madt *madt = acpi_get_table("APIC", 0);

    if (madt == NULL)
        return NULL;

    uint64_t bsp_mpidr;
    asm volatile ("mrs %0, mpidr_el1" : "=r"(bsp_mpidr));

    // This bit is Res1 in the system reg, but not included in the MPIDR from MADT
    bsp_mpidr &= ~((uint64_t)1 << 31);

    *_bsp_mpidr = bsp_mpidr;

    printv("smp: BSP MPIDR is %X\n", bsp_mpidr);

    *cpu_count = 0;

    // Count the MAX of startable APs and allocate accordingly
    size_t max_cpus = 0;

    for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
      (uintptr_t)madt_ptr < (uintptr_t)madt + madt->header.length;
      madt_ptr += *(madt_ptr + 1)) {
        switch (*madt_ptr) {
            case 11: {
                // GIC CPU Interface
                struct madt_gicc *gicc = (void *)madt_ptr;

                // Check if we can actually try to start the AP
                if (gicc->flags & 1)
                    max_cpus++;

                continue;
            }
        }
    }

    struct limine_mp_info *ret = ext_mem_alloc(max_cpus * sizeof(struct limine_mp_info));
    *cpu_count = 0;

    // Try to start all APs
    for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
      (uintptr_t)madt_ptr < (uintptr_t)madt + madt->header.length;
      madt_ptr += *(madt_ptr + 1)) {
        switch (*madt_ptr) {
            case 11: {
                // GIC CPU Interface
                struct madt_gicc *gicc = (void *)madt_ptr;

                // Check if we can actually try to start the AP
                if (!(gicc->flags & 1))
                    continue;

                struct limine_mp_info *info_struct = &ret[*cpu_count];

                info_struct->processor_id = gicc->acpi_uid;
                info_struct->mpidr = gicc->mpidr;

                // Do not try to restart the BSP
                if (gicc->mpidr == bsp_mpidr) {
                    (*cpu_count)++;
                    continue;
                }

                printv("smp: Found candidate AP for bring-up. Interface no.: %x, MPIDR: %X\n", gicc->iface_no, gicc->mpidr);

                // Try to start the AP
                if (!try_start_ap(boot_method, gicc->parking_addr, info_struct,
                                  (uint64_t)(uintptr_t)pagemap.top_level[0],
                                  (uint64_t)(uintptr_t)pagemap.top_level[1],
                                  mair, tcr, sctlr, hhdm_offset)) {
                    print("smp: FAILED to bring-up AP\n");
                    continue;
                }

                printv("smp: Successfully brought up AP\n");

                (*cpu_count)++;
                continue;
            }
        }
    }

    return ret;
}

static struct limine_mp_info *try_dtb_smp(const char *config,
                                           size_t   *cpu_count,
                                           uint64_t *_bsp_mpidr,
                                           pagemap_t pagemap,
                                           uint64_t  mair,
                                           uint64_t  tcr,
                                           uint64_t  sctlr,
                                           uint64_t  hhdm_offset) {
    void *dtb = get_device_tree_blob(config, 0);

    uint64_t bsp_mpidr;
    asm volatile ("mrs %0, mpidr_el1" : "=r"(bsp_mpidr));

    // This bit is Res1 in the system reg, but not included in the MPIDR from DT
    bsp_mpidr &= ~((uint64_t)1 << 31);

    *_bsp_mpidr = bsp_mpidr;

    printv("smp: BSP MPIDR is %X\n", bsp_mpidr);

    *cpu_count = 0;

    int cpus = fdt_path_offset(dtb, "/cpus");
    if (cpus < 0) {
        printv("smp: failed to find /cpus node: %s\n", fdt_strerror(cpus));
        return NULL;
    }

    int psci = fdt_path_offset(dtb, "/psci");

    if (psci > 0 && !fdt_node_check_compatible(dtb, psci, "arm,psci")) {
        const void *prop;
        if (!(prop = fdt_getprop(dtb, psci, "cpu_on", NULL))) {
            printv("smp: failed to find PSCI cpu_on prop\n");
            return NULL;
        }

        const uint8_t *bytes = prop;

        psci_cpu_on = ((uint64_t)bytes[0] << 24)
            | ((uint64_t)bytes[1] << 16)
            | ((uint64_t)bytes[2] << 8)
            | ((uint64_t)bytes[3]);
    }

    int address_cells = fdt_address_cells(dtb, cpus);
    if (address_cells < 0) {
        printv("smp: fdt_address_cells failed: %s\n", fdt_strerror(address_cells));
        return NULL;
    }
    if (address_cells > 2) {
        printv("smp: illegal #address-cells value: %d\n", address_cells);
        return NULL;
    }

    uint64_t max_cpus = 0;
    int node;
    fdt_for_each_subnode(node, dtb, cpus) {
        const void *prop;

        if (!(prop = fdt_getprop(dtb, node, "device_type", NULL)) || strcmp(prop, "cpu")) {
            continue;
        }

        if (!(prop = fdt_getprop(dtb, node, "reg", NULL))) {
            continue;
        }

        max_cpus++;
    }

    struct limine_mp_info *ret = ext_mem_alloc(max_cpus * sizeof(struct limine_mp_info));

    fdt_for_each_subnode(node, dtb, cpus) {
        const void *prop;

        if (!(prop = fdt_getprop(dtb, node, "device_type", NULL)) || strcmp(prop, "cpu")) {
            continue;
        }

        if (!(prop = fdt_getprop(dtb, node, "reg", NULL))) {
            continue;
        }

        uint64_t mpidr = 0;

        if (address_cells == 1) {
            const uint8_t *bytes = prop;

            mpidr = ((uint64_t)bytes[0] << 24)
                | ((uint64_t)bytes[1] << 16)
                | ((uint64_t)bytes[2] << 8)
                | ((uint64_t)bytes[3]);
        } else if (address_cells == 2) {
            const uint8_t *bytes = prop;

            mpidr = ((uint64_t)bytes[3] << 32)
                | ((uint64_t)bytes[4] << 24)
                | ((uint64_t)bytes[5] << 16)
                | ((uint64_t)bytes[6] << 8)
                | ((uint64_t)bytes[7]);
        }


        struct limine_mp_info *info_struct = &ret[*cpu_count];

        info_struct->processor_id = 0;
        info_struct->mpidr = mpidr;

        // Do not try to restart the BSP
        if (mpidr == bsp_mpidr) {
            (*cpu_count)++;
            continue;
        }

        if (!(prop = fdt_getprop(dtb, node, "enable-method", NULL))) {
            printv("smp: missing enable-method\n");
            continue;
        }

        int boot_method = -1;
        uint64_t method_ptr = 0;

        if (!strcmp(prop, "psci")) {
            if (psci < 0) {
                printv("smp: failed to find /psci: %s\n", fdt_strerror(psci));
                continue;
            }

            const void *psci_method = fdt_getprop(dtb, psci, "method", NULL);

            if (!strcmp(psci_method, "smc")) {
                boot_method = BOOT_WITH_PSCI_SMC;
            } else if (!strcmp(psci_method, "hvc")) {
                boot_method = BOOT_WITH_PSCI_HVC;
            } else {
                printv("smp: illegal PSCI method: '%s'\n", psci_method);
            }

        } else if (!strcmp(prop, "spin-table")) {
            boot_method = BOOT_WITH_SPIN_TBL;

            if (!(prop = fdt_getprop(dtb, node, "cpu-release-addr", NULL))) {
                printv("smp: missing cpu-release-addr\n");
                continue;
            }

            const uint8_t *bytes = prop;

            method_ptr = ((uint64_t)bytes[0] << 56)
                | ((uint64_t)bytes[1] << 48)
                | ((uint64_t)bytes[2] << 40)
                | ((uint64_t)bytes[3] << 32)
                | ((uint64_t)bytes[4] << 24)
                | ((uint64_t)bytes[5] << 16)
                | ((uint64_t)bytes[6] << 8)
                | ((uint64_t)bytes[7]);
        } else {
            printv("smp: illegal enable-method: '%s'\n", prop);
            continue;
        }

        printv("smp: Found candidate AP for bring-up. MPIDR: %X\n", mpidr);

        // Try to start the AP
        if (!try_start_ap(boot_method, method_ptr, info_struct,
                                        (uint64_t)(uintptr_t)pagemap.top_level[0],
                                        (uint64_t)(uintptr_t)pagemap.top_level[1],
                                        mair, tcr, sctlr, hhdm_offset)) {
            print("smp: FAILED to bring-up AP\n");
            continue;
        }

        printv("smp: Successfully brought up AP\n");

        (*cpu_count)++;
    }

    return ret;
}


struct limine_mp_info *init_smp(const char *config,
                                 size_t   *cpu_count,
                                 uint64_t *bsp_mpidr,
                                 pagemap_t pagemap,
                                 uint64_t  mair,
                                 uint64_t  tcr,
                                 uint64_t  sctlr,
                                 uint64_t  hhdm_offset) {
    struct limine_mp_info *info = NULL;

    if (acpi_get_rsdp() && (info = try_acpi_smp(
                                    cpu_count, bsp_mpidr, pagemap,
                                    mair, tcr, sctlr, hhdm_offset)))
        return info;

    // No RSDP means no ACPI, try device trees in that case.
    if (get_device_tree_blob(config, 0)
                    && (info = try_dtb_smp(
                                config, cpu_count, bsp_mpidr, pagemap,
                                mair, tcr, sctlr, hhdm_offset)))
        return info;

    printv("Failed to figure out how to start APs.");

    return NULL;
}

#elif defined (__riscv)

struct trampoline_passed_info {
    uint64_t smp_tpl_booted_flag;
    uint64_t smp_tpl_satp;
    uint64_t smp_tpl_info_struct;
    uint64_t smp_tpl_hhdm_offset;
};

static bool smp_start_ap(size_t hartid, size_t satp, struct limine_mp_info *info_struct,
                         uint64_t hhdm_offset) {
    static struct trampoline_passed_info passed_info;

    passed_info.smp_tpl_booted_flag = 0;
    passed_info.smp_tpl_satp        = satp;
    passed_info.smp_tpl_info_struct = (uint64_t)info_struct;
    passed_info.smp_tpl_hhdm_offset = hhdm_offset;

    asm volatile ("" ::: "memory");

    struct sbiret ret = sbi_hart_start(hartid, (size_t)smp_trampoline_start, (size_t)&passed_info);
    if (ret.error != SBI_SUCCESS)
        return false;

    for (int i = 0; i < 1000000; i++) {
        if (locked_read(&passed_info.smp_tpl_booted_flag) == 1)
            return true;

        delay(100000);
    }

    return false;
}

struct limine_mp_info *init_smp(size_t *cpu_count, pagemap_t pagemap, uint64_t hhdm_offset) {
    size_t num_cpus = 0;
    for (struct riscv_hart *hart = hart_list; hart != NULL; hart = hart->next) {
        if (!(hart->flags & RISCV_HART_COPROC)) {
            num_cpus += 1;
        }
    }

    struct limine_mp_info *ret = ext_mem_alloc(num_cpus * sizeof(struct limine_mp_info));

    *cpu_count = 0;
    for (struct riscv_hart *hart = hart_list; hart != NULL; hart = hart->next) {
        if (hart->flags & RISCV_HART_COPROC) {
            continue;
        }
        struct limine_mp_info *info_struct = &ret[*cpu_count];

        info_struct->hartid = hart->hartid;
        info_struct->processor_id = hart->acpi_uid;

        // Don't try to start the BSP.
        if (hart->hartid == bsp_hartid) {
            *cpu_count += 1;
            continue;
        }

        printv("smp: Found candidate AP for bring-up. Hart ID: %u\n", hart->hartid);

        // Try to start the AP.
        size_t satp = make_satp(pagemap.paging_mode, pagemap.top_level);
        if (!smp_start_ap(hart->hartid, satp, info_struct, hhdm_offset)) {
            print("smp: FAILED to bring-up AP\n");
            continue;
        }

        (*cpu_count)++;
        continue;
    }

    return ret;
}

#elif defined (__loongarch64)
#else
#error Unknown architecture
#endif
