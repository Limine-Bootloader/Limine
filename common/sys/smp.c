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
#if defined (__aarch64__) || defined(__loongarch__)
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
    uint64_t smp_tpl_lapic_setup;
} __attribute__((packed));

bool smp_configure_apic = false;

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
    passed_info->smp_tpl_temp_stack = (uint64_t)(uintptr_t)temp_stack + 8192;
    passed_info->smp_tpl_lapic_setup = smp_configure_apic
        ? (uint64_t)(uintptr_t)lapic_configure_handoff_state : 0;

    asm volatile ("" ::: "memory");

    // Send the INIT IPI
    if (x2apic) {
        x2apic_write(LAPIC_REG_ICR0, ((uint64_t)lapic_id << 32) | 0x4500);
    } else {
        lapic_icr_wait();
        lapic_write(LAPIC_REG_ICR1, lapic_id << 24);
        lapic_write(LAPIC_REG_ICR0, 0x4500);
    }
    stall(10000);

    // Send two Startup IPIs per Intel SDM recommendation (Vol 3, 8.4.4.1)
    for (int j = 0; j < 2; j++) {
        if (x2apic) {
            x2apic_write(LAPIC_REG_ICR0, ((uint64_t)lapic_id << 32) |
                                         ((size_t)trampoline / 4096) | 0x4600);
        } else {
            lapic_icr_wait();
            lapic_write(LAPIC_REG_ICR1, lapic_id << 24);
            lapic_write(LAPIC_REG_ICR0, ((size_t)trampoline / 4096) | 0x4600);
        }
        if (j == 0) {
            stall(200);
        }
    }

    if (!x2apic) {
        lapic_icr_wait();
    }

    for (int i = 0; i < 100; i++) {
        if (locked_read(&passed_info->smp_tpl_booted_flag) == 1) {
            return true;
        }
        stall(10000);
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

    uint32_t bsp_lapic_id;

    // If x2APIC already enabled by firmware, try to revert to xAPIC
    if (rdmsr(0x1b) & (1 << 10)) {
        if (!x2apic) {
            if (!x2apic_disable()) {
                panic(false, "smp: Kernel does not support x2APIC and x2APIC cannot be disabled");
            }
            printv("smp: Firmware had x2APIC enabled, reverted to xAPIC mode\n");
        }
    }

    x2apic = x2apic && x2apic_enable();

    if (x2apic) {
        bsp_lapic_id = x2apic_read(LAPIC_REG_ID);
    } else {
        bsp_lapic_id = lapic_read(LAPIC_REG_ID) >> 24;
    }

    *_bsp_lapic_id = bsp_lapic_id;

    *cpu_count = 0;

    // Count the MAX of startable APs and allocate accordingly
    size_t max_cpus = 0;

    for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
      (uintptr_t)madt_ptr + 1 < (uintptr_t)madt + madt->header.length;
      madt_ptr += *(madt_ptr + 1)) {
        // Skip zero-length or out-of-bounds MADT entries
        if (*(madt_ptr + 1) == 0
         || (uintptr_t)madt_ptr + *(madt_ptr + 1) > (uintptr_t)madt + madt->header.length) {
            break;
        }
        switch (*madt_ptr) {
            case 0: {
                // Processor local xAPIC
                if (*(madt_ptr + 1) < sizeof(struct madt_lapic))
                    continue;

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

                if (*(madt_ptr + 1) < sizeof(struct madt_x2apic))
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

    struct limine_mp_info *ret = ext_mem_alloc_counted(max_cpus, sizeof(struct limine_mp_info));
    *cpu_count = 0;

    // Try to start all APs
    mtrr_save();

    for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
      (uintptr_t)madt_ptr + 1 < (uintptr_t)madt + madt->header.length;
      madt_ptr += *(madt_ptr + 1)) {
        // Skip zero-length or out-of-bounds MADT entries
        if (*(madt_ptr + 1) == 0
         || (uintptr_t)madt_ptr + *(madt_ptr + 1) > (uintptr_t)madt + madt->header.length) {
            break;
        }
        switch (*madt_ptr) {
            case 0: {
                // Processor local xAPIC
                if (*(madt_ptr + 1) < sizeof(struct madt_lapic))
                    continue;

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

                // Set up per-AP LINT values before starting
                if (smp_configure_apic) {
                    lapic_prep_lint(madt, lapic->acpi_processor_uid, x2apic);
                }

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

                if (*(madt_ptr + 1) < sizeof(struct madt_x2apic))
                    continue;

                struct madt_x2apic *x2lapic = (void *)madt_ptr;

                // Check if we can actually try to start the AP
                if (!((x2lapic->flags & 1) ^ ((x2lapic->flags >> 1) & 1)))
                    continue;

                struct limine_mp_info *info_struct = &ret[*cpu_count];

                info_struct->processor_id = x2lapic->acpi_processor_uid;
                info_struct->lapic_id = x2lapic->x2apic_id;

                // Do not try to restart the BSP
                if (x2lapic->x2apic_id == bsp_lapic_id) {
                    (*cpu_count)++;
                    continue;
                }

                printv("smp: [x2APIC] Found candidate AP for bring-up. LAPIC ID: %u\n", x2lapic->x2apic_id);

                // Set up per-AP LINT values before starting
                if (smp_configure_apic) {
                    lapic_prep_lint(madt, x2lapic->acpi_processor_uid, true);
                }

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
    uint64_t smp_tpl_ap_el;

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
    passed_info->smp_tpl_ap_el       = 0;
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

    clean_dcache_poc((uintptr_t)trampoline, (uintptr_t)trampoline + smp_trampoline_size);
    inval_icache_pou((uintptr_t)trampoline, (uintptr_t)trampoline + smp_trampoline_size);

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
            uint64_t ap_el = locked_read(&passed_info->smp_tpl_ap_el);
            uint64_t bsp_el = current_el();
            if (ap_el != bsp_el) {
                panic(false, "smp: AP started at EL%u but BSP is at EL%u",
                      (uint32_t)ap_el, (uint32_t)bsp_el);
            }
            return true;
        }
        stall(100);
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

    // Check FADT length before accessing ARM boot flags at offset 129
    uint32_t fadt_length;
    memcpy(&fadt_length, fadt + 4, 4);
    if (fadt_length >= 131) {
        // Read the single field from the FADT without defining a struct for the whole table
        uint16_t arm_boot_args;
        memcpy(&arm_boot_args, fadt + 129, 2);

        if (arm_boot_args & 1) // PSCI compliant?
            boot_method = arm_boot_args & 2 ? BOOT_WITH_PSCI_HVC : BOOT_WITH_PSCI_SMC;
    }

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
      (uintptr_t)madt_ptr + 1 < (uintptr_t)madt + madt->header.length;
      madt_ptr += *(madt_ptr + 1)) {
        if (*(madt_ptr + 1) == 0
         || (uintptr_t)madt_ptr + *(madt_ptr + 1) > (uintptr_t)madt + madt->header.length) {
            break;
        }
        switch (*madt_ptr) {
            case 11: {
                // GIC CPU Interface
                if (*(madt_ptr + 1) < sizeof(struct madt_gicc))
                    continue;

                struct madt_gicc *gicc = (void *)madt_ptr;

                // Check if we can actually try to start the AP
                if (gicc->flags & 1)
                    max_cpus++;

                continue;
            }
        }
    }

    struct limine_mp_info *ret = ext_mem_alloc_counted(max_cpus, sizeof(struct limine_mp_info));
    *cpu_count = 0;

    // Try to start all APs
    for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
      (uintptr_t)madt_ptr + 1 < (uintptr_t)madt + madt->header.length;
      madt_ptr += *(madt_ptr + 1)) {
        if (*(madt_ptr + 1) == 0
         || (uintptr_t)madt_ptr + *(madt_ptr + 1) > (uintptr_t)madt + madt->header.length) {
            break;
        }
        switch (*madt_ptr) {
            case 11: {
                // GIC CPU Interface
                if (*(madt_ptr + 1) < sizeof(struct madt_gicc))
                    continue;

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

    if (*cpu_count == 0) {
        pmm_free(ret, max_cpus * sizeof(struct limine_mp_info));
        return NULL;
    }

    return ret;
}

static struct limine_mp_info *try_dtb_smp( void *dtb,
                                           size_t   *cpu_count,
                                           uint64_t *_bsp_mpidr,
                                           pagemap_t pagemap,
                                           uint64_t  mair,
                                           uint64_t  tcr,
                                           uint64_t  sctlr,
                                           uint64_t  hhdm_offset) {
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
        int prop_len;
        const void *prop;
        if (!(prop = fdt_getprop(dtb, psci, "cpu_on", &prop_len)) || prop_len < 4) {
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
    if (address_cells < 1) {
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

    struct limine_mp_info *ret = ext_mem_alloc_counted(max_cpus, sizeof(struct limine_mp_info));

    fdt_for_each_subnode(node, dtb, cpus) {
        const void *prop;
        int prop_len;

        if (!(prop = fdt_getprop(dtb, node, "device_type", NULL)) || strcmp(prop, "cpu")) {
            continue;
        }

        if (!(prop = fdt_getprop(dtb, node, "reg", &prop_len)) || prop_len < address_cells * 4) {
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

            if (psci_method == NULL) {
                printv("smp: PSCI method property not found\n");
                continue;
            } else if (!strcmp(psci_method, "smc")) {
                boot_method = BOOT_WITH_PSCI_SMC;
            } else if (!strcmp(psci_method, "hvc")) {
                boot_method = BOOT_WITH_PSCI_HVC;
            } else {
                printv("smp: illegal PSCI method: '%s'\n", psci_method);
                continue;
            }

        } else if (!strcmp(prop, "spin-table")) {
            boot_method = BOOT_WITH_SPIN_TBL;

            if (!(prop = fdt_getprop(dtb, node, "cpu-release-addr", &prop_len)) || prop_len < 8) {
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
    void *dtb = get_device_tree_blob(config, 0, false);
    if (dtb) {
        info = try_dtb_smp(dtb,
                           cpu_count, bsp_mpidr, pagemap,
                           mair, tcr, sctlr, hhdm_offset);
        pmm_free(dtb, fdt_totalsize(dtb));
        return info;
    }

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

    asm volatile ("fence w,w" ::: "memory");

    struct sbiret ret = sbi_hart_start(hartid, (size_t)smp_trampoline_start, (size_t)&passed_info);
    if (ret.error != SBI_SUCCESS)
        return false;

    for (int i = 0; i < 1000000; i++) {
        if (locked_read(&passed_info.smp_tpl_booted_flag) == 1)
            return true;

        stall(100);
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

    struct limine_mp_info *ret = ext_mem_alloc_counted(num_cpus, sizeof(struct limine_mp_info));

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

        printv("smp: Found candidate AP for bring-up. Hart ID: %U\n", (uint64_t)hart->hartid);

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

enum {
    LOONGARCH_CSR_CPUID = 0x20,

    LOONGARCH_IOCSR_IPI_SEND = 0x1040,
    LOONGARCH_IOCSR_MBUF_SEND = 0x1048,

    IOCSR_IPI_SEND_BLOCKING_BIT = 31,
    IOCSR_IPI_SEND_CPU_SHIFT    = 16,
    IOCSR_IPI_SEND_IP_SHIFT     = 0,

    IOCSR_MBUF_SEND_BLOCKING_BIT = 31,
    IOCSR_MBUF_SEND_CPU_SHIFT    = 16,
    IOCSR_MBUF_SEND_BOX_SHIFT    = 2,

    SMP_BOOT_CPU = 0x1,

    MADT_ENTRY_CORE_PIC = 17
};

struct trampoline_passed_info {
    uint64_t smp_tpl_booted_flag;
    uint64_t smp_tpl_info_struct;
    uint64_t smp_tpl_pgd_low;
    uint64_t smp_tpl_pgd_high;
    uint64_t smp_tpl_hhdm_offset;
    uint64_t smp_tpl_temp_stack;
};

struct trampoline_passed_info loongarch_smp_passed_info;

static inline uint32_t loongarch_phys_id(void) {
    return csr_read32(LOONGARCH_CSR_CPUID);
}

static inline bool core_pic_startable(uint32_t flags) {
    return (flags & MADT_CORE_PIC_ENABLED)
        || (flags & MADT_CORE_PIC_ONLINE_CAPABLE);
}

static void csr_mail_send(uint64_t data, int cpu, int mailbox) {
	uint64_t val;

    // High 32bit
	val = ((uint64_t)1 << IOCSR_MBUF_SEND_BLOCKING_BIT);
	val |= (((mailbox << 1) + 1) << IOCSR_MBUF_SEND_BOX_SHIFT);
	val |= (cpu << IOCSR_MBUF_SEND_CPU_SHIFT);
	val |= (data & 0xFFFFFFFF00000000);
	iocsr_write64(val, LOONGARCH_IOCSR_MBUF_SEND);

    // Low 32bit
	val = ((uint64_t)1 << IOCSR_MBUF_SEND_BLOCKING_BIT);
	val |= ((mailbox << 1) << IOCSR_MBUF_SEND_BOX_SHIFT);
	val |= (cpu << IOCSR_MBUF_SEND_CPU_SHIFT);
	val |= (data << 32);
	iocsr_write64(val, LOONGARCH_IOCSR_MBUF_SEND);
};

static void smp_send_ipi(uint32_t phys_id, uint32_t action) {
    uint32_t val = ((uint32_t)1 << IOCSR_IPI_SEND_BLOCKING_BIT)
                 | (phys_id << IOCSR_IPI_SEND_CPU_SHIFT)
                 | (action << IOCSR_IPI_SEND_IP_SHIFT);

    iocsr_write32(val, LOONGARCH_IOCSR_IPI_SEND);
}

static bool smp_start_ap(uint32_t phys_id, struct limine_mp_info *info_struct,
                         uint64_t pgd_low, uint64_t pgd_high,
                         uint64_t hhdm_offset) {
    static void *temp_stack =NULL;
    if (temp_stack == NULL) {
        temp_stack = ext_mem_alloc(8192);
    }

    loongarch_smp_passed_info.smp_tpl_booted_flag = 0;
    loongarch_smp_passed_info.smp_tpl_info_struct = (uint64_t)(uintptr_t)info_struct;
    loongarch_smp_passed_info.smp_tpl_pgd_low     = pgd_low;
    loongarch_smp_passed_info.smp_tpl_pgd_high    = pgd_high;
    loongarch_smp_passed_info.smp_tpl_hhdm_offset = hhdm_offset;
    loongarch_smp_passed_info.smp_tpl_temp_stack  = (uint64_t)(uintptr_t)temp_stack + 8192;

    asm volatile ("dbar 0" ::: "memory");

    uint64_t trampoline_entry = (uint64_t)(uintptr_t)smp_trampoline_start;

    // Mailbox 0 and 1 carry the low and high 32 bits of the AP entry point.
    csr_mail_send(trampoline_entry, phys_id, 0);
    smp_send_ipi(phys_id, SMP_BOOT_CPU);

    for (int i = 0; i < 1000000; i++) {
        if (locked_read(&loongarch_smp_passed_info.smp_tpl_booted_flag) == 1)
            return true;
        stall(100);
    }

    return false;
}

static struct limine_mp_info *try_acpi_smp(size_t *cpu_count, uint32_t *bsp_phys_id,
                                           pagemap_t pagemap, uint64_t hhdm_offset) {
    struct madt *madt = acpi_get_table("APIC", 0);
    if (madt == NULL)
        return NULL;

    *bsp_phys_id = loongarch_phys_id();
    *cpu_count = 0;

    size_t max_cpus = 0;

    for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
         (uintptr_t)madt_ptr + 1 < (uintptr_t)madt + madt->header.length;
         madt_ptr += *(madt_ptr + 1)) {
        if (*(madt_ptr + 1) == 0
         || (uintptr_t)madt_ptr + *(madt_ptr + 1) > (uintptr_t)madt + madt->header.length)
            break;

        if (*madt_ptr != MADT_ENTRY_CORE_PIC)
            continue;

        if (*(madt_ptr + 1) < sizeof(struct madt_core_pic))
            continue;

        struct madt_core_pic *core_pic = (void *)madt_ptr;

        if (core_pic_startable(core_pic->flags))
            max_cpus++;
    }

    if (max_cpus == 0)
        return NULL;

    struct limine_mp_info *ret = ext_mem_alloc_counted(max_cpus, sizeof(struct limine_mp_info));

    for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
         (uintptr_t)madt_ptr + 1 < (uintptr_t)madt + madt->header.length;
         madt_ptr += *(madt_ptr + 1)) {
        if (*(madt_ptr + 1) == 0
         || (uintptr_t)madt_ptr + *(madt_ptr + 1) > (uintptr_t)madt + madt->header.length)
            break;

        if (*madt_ptr != MADT_ENTRY_CORE_PIC)
            continue;

        if (*(madt_ptr + 1) < sizeof(struct madt_core_pic))
            continue;

        struct madt_core_pic *core_pic = (void *)madt_ptr;

        if (!core_pic_startable(core_pic->flags))
            continue;

        struct limine_mp_info *info_struct = &ret[*cpu_count];
        info_struct->processor_id = core_pic->acpi_processor_uid;
        info_struct->phys_id = core_pic->core_id;

        // Do not try to restart the BSP.
        if (core_pic->core_id == *bsp_phys_id) {
            (*cpu_count)++;
            continue;
        }

        printv("smp: Found candidate AP for bring-up. Core ID: %u\n", core_pic->core_id);

        if (!smp_start_ap(core_pic->core_id, info_struct,
                          (uint64_t)(uintptr_t)pagemap.pgd[0],
                          (uint64_t)(uintptr_t)pagemap.pgd[1],
                          hhdm_offset)) {
            print("smp: FAILED to bring-up AP\n");
            continue;
        }

        printv("smp: Successfully brought up AP\n");
        (*cpu_count)++;
    }

    if (*cpu_count == 0) {
        pmm_free(ret, max_cpus * sizeof(struct limine_mp_info));
        return NULL;
    }

    return ret;
}

static struct limine_mp_info *try_dtb_smp(void *dtb, size_t *cpu_count,
                                          uint32_t *bsp_phys_id,
                                          pagemap_t pagemap,
                                          uint64_t hhdm_offset) {
    int cpus = fdt_path_offset(dtb, "/cpus");
    if (cpus < 0) {
        printv("smp: failed to find /cpus node: %s\n", fdt_strerror(cpus));
        return NULL;
    }

    int address_cells = fdt_address_cells(dtb, cpus);
    if (address_cells < 1) {
        printv("smp: fdt_address_cells failed: %s\n", fdt_strerror(address_cells));
        return NULL;
    }
    if (address_cells > 2) {
        printv("smp: illegal #address-cells value: %d\n", address_cells);
        return NULL;
    }

    *bsp_phys_id = loongarch_phys_id();
    *cpu_count = 0;

    size_t max_cpus = 0;
    int node;
    fdt_for_each_subnode(node, dtb, cpus) {
        const void *prop;
        int prop_len;

        if (!(prop = fdt_getprop(dtb, node, "device_type", NULL)) || strcmp(prop, "cpu"))
            continue;

        if (!(prop = fdt_getprop(dtb, node, "reg", &prop_len)) || prop_len < address_cells * 4)
            continue;

        uint64_t phys_id = 0;
        const uint8_t *bytes = prop;

        if (address_cells == 1) {
            phys_id = ((uint64_t)bytes[0] << 24)
                    | ((uint64_t)bytes[1] << 16)
                    | ((uint64_t)bytes[2] << 8)
                    | ((uint64_t)bytes[3] << 0);
        } else {
            phys_id = ((uint64_t)bytes[0] << 56)
                    | ((uint64_t)bytes[1] << 48)
                    | ((uint64_t)bytes[2] << 40)
                    | ((uint64_t)bytes[3] << 32)
                    | ((uint64_t)bytes[4] << 24)
                    | ((uint64_t)bytes[5] << 16)
                    | ((uint64_t)bytes[6] << 8)
                    | ((uint64_t)bytes[7] << 0);
        }

        if (phys_id > UINT32_MAX) {
            printv("smp: core id %U does not fit in 32 bits, skipping\n", phys_id);
            continue;
        }

        max_cpus++;
    }

    if (max_cpus == 0)
        return NULL;

    struct limine_mp_info *ret = ext_mem_alloc_counted(max_cpus, sizeof(struct limine_mp_info));

    fdt_for_each_subnode(node, dtb, cpus) {
        const void *prop;
        int prop_len;

        if (!(prop = fdt_getprop(dtb, node, "device_type", NULL)) || strcmp(prop, "cpu"))
            continue;

        if (!(prop = fdt_getprop(dtb, node, "reg", &prop_len)) || prop_len < address_cells * 4)
            continue;

        uint64_t phys_id = 0;
        const uint8_t *bytes = prop;

        if (address_cells == 1) {
            phys_id = ((uint64_t)bytes[0] << 24)
                    | ((uint64_t)bytes[1] << 16)
                    | ((uint64_t)bytes[2] << 8)
                    | ((uint64_t)bytes[3] << 0);
        } else {
            phys_id = ((uint64_t)bytes[0] << 56)
                    | ((uint64_t)bytes[1] << 48)
                    | ((uint64_t)bytes[2] << 40)
                    | ((uint64_t)bytes[3] << 32)
                    | ((uint64_t)bytes[4] << 24)
                    | ((uint64_t)bytes[5] << 16)
                    | ((uint64_t)bytes[6] << 8)
                    | ((uint64_t)bytes[7] << 0);
        }

        if (phys_id > UINT32_MAX) {
            printv("smp: core id %U does not fit in 32 bits, skipping\n", phys_id);
            continue;
        }

        struct limine_mp_info *info_struct = &ret[*cpu_count];
        info_struct->processor_id = 0;
        info_struct->phys_id = phys_id;

        // Do not try to restart the BSP.
        if (phys_id == *bsp_phys_id) {
            (*cpu_count)++;
            continue;
        }

        printv("smp: Found candidate AP for bring-up. Core ID: %U\n", phys_id);

        if (!smp_start_ap((uint32_t)phys_id, info_struct,
                          (uint64_t)(uintptr_t)pagemap.pgd[0],
                          (uint64_t)(uintptr_t)pagemap.pgd[1],
                          hhdm_offset)) {
            print("smp: FAILED to bring-up AP\n");
            continue;
        }

        printv("smp: Successfully brought up AP\n");
        (*cpu_count)++;
    }

    if (*cpu_count == 0) {
        pmm_free(ret, max_cpus * sizeof(struct limine_mp_info));
        return NULL;
    }

    return ret;
}

struct limine_mp_info *init_smp(size_t *cpu_count, uint32_t *bsp_phys_id,
                                pagemap_t pagemap, uint64_t hhdm_offset) {
    struct limine_mp_info *info = NULL;

    if (acpi_get_rsdp() && (info = try_acpi_smp(cpu_count, bsp_phys_id, pagemap, hhdm_offset)))
        return info;

    void *dtb = get_device_tree_blob(NULL, 0, false);
    if (dtb) {
        info = try_dtb_smp(dtb, cpu_count, bsp_phys_id, pagemap, hhdm_offset);
        pmm_free(dtb, fdt_totalsize(dtb));
        return info;
    }

    printv("Failed to figure out how to start APs.");

    return NULL;
}

#else
#error Unknown architecture
#endif
