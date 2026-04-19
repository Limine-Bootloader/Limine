#if defined (__x86_64__) || defined (__i386__)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/lapic.h>
#include <sys/cpu.h>
#include <lib/misc.h>
#include <lib/acpi.h>
#include <mm/pmm.h>

#define LAPIC_REG_LVT_CMCI    0x2f0
#define LAPIC_REG_LVT_TIMER   0x320
#define LAPIC_REG_LVT_THERMAL 0x330
#define LAPIC_REG_LVT_PMC     0x340
#define LAPIC_REG_LVT_LINT0   0x350
#define LAPIC_REG_LVT_LINT1   0x360
#define LAPIC_REG_LVT_ERROR   0x370
#define LAPIC_REG_SVR         0x0f0
#define LAPIC_REG_TPR         0x080
#define LAPIC_REG_VERSION     0x030

static uint32_t pending_lint0 = UINT32_MAX; // no override
static uint32_t pending_lint1 = UINT32_MAX; // no override

static uint32_t lapic_madt_nmi_flags_to_lvt(uint16_t flags) {
    uint32_t lvt = 0x10400; // masked + NMI delivery mode

    // Polarity: bits 1:0 of flags
    uint8_t polarity = flags & 0x3;
    if (polarity == 0x3) {
        lvt |= (1 << 13); // active low
    }
    // 0b00 (conforms) and 0b01 (active high) leave bit 13 clear

    // Trigger mode: bits 3:2 of flags
    uint8_t trigger = (flags >> 2) & 0x3;
    if (trigger == 0x3) {
        lvt |= (1 << 15); // level triggered
    }
    // 0b00 (conforms) and 0b01 (edge) leave bit 15 clear

    return lvt;
}

void lapic_prep_lint(struct madt *madt, uint32_t acpi_uid, bool x2apic) {
    pending_lint0 = UINT32_MAX; // no override
    pending_lint1 = UINT32_MAX; // no override

    // Walk MADT entries looking for NMI entries
    for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
      (uintptr_t)madt_ptr + 1 < (uintptr_t)madt + madt->header.length;
      madt_ptr += *(madt_ptr + 1)) {
        if (*(madt_ptr + 1) == 0
         || (uintptr_t)madt_ptr + *(madt_ptr + 1) > (uintptr_t)madt + madt->header.length) {
            break;
        }
        switch (*madt_ptr) {
            case 4: {
                // Local APIC NMI
                if (*(madt_ptr + 1) < sizeof(struct madt_lapic_nmi)) {
                    continue;
                }

                struct madt_lapic_nmi *nmi = (void *)madt_ptr;

                // Match all processors (0xff) or specific UID.
                if (nmi->acpi_processor_uid != 0xff
                 && (acpi_uid > 0xfe || nmi->acpi_processor_uid != acpi_uid)) {
                    continue;
                }

                uint32_t lvt = lapic_madt_nmi_flags_to_lvt(nmi->flags);
                if (nmi->lint == 0) {
                    pending_lint0 = lvt;
                } else if (nmi->lint == 1) {
                    pending_lint1 = lvt;
                }
                continue;
            }
            case 0x0a: {
                // Local x2APIC NMI
                if (!x2apic) {
                    continue;
                }
                if (*(madt_ptr + 1) < sizeof(struct madt_x2apic_nmi)) {
                    continue;
                }

                struct madt_x2apic_nmi *nmi = (void *)madt_ptr;

                // Match all processors (0xffffffff) or specific UID
                if (nmi->acpi_processor_uid != 0xffffffff && nmi->acpi_processor_uid != acpi_uid) {
                    continue;
                }

                uint32_t lvt = lapic_madt_nmi_flags_to_lvt(nmi->flags);
                if (nmi->lint == 0) {
                    pending_lint0 = lvt;
                } else if (nmi->lint == 1) {
                    pending_lint1 = lvt;
                }
                continue;
            }
        }
    }
}

static bool lvt_should_mask(uint32_t lvt) {
    switch ((lvt >> 8) & 7) {
        case 0b000: // Fixed
        case 0b001: // Lowest Priority
        case 0b100: // NMI
        case 0b111: // ExtINT
            return true;
        default:    // SMI, INIT, Reserved
            return false;
    }
}

void lapic_configure_handoff_state(void) {
    bool is_x2 = !!(rdmsr(0x1b) & (1 << 10));

    uint32_t max_lvt;
    if (is_x2) {
        max_lvt = (x2apic_read(LAPIC_REG_VERSION) >> 16) & 0xff;
    } else {
        max_lvt = (lapic_read(LAPIC_REG_VERSION) >> 16) & 0xff;
    }

    uint32_t lvt;

    if (is_x2) {
        x2apic_write(LAPIC_REG_SVR, 0x1ff);
        x2apic_write(LAPIC_REG_TPR, 0);
        if (max_lvt >= 6) {
            lvt = x2apic_read(LAPIC_REG_LVT_CMCI);
            if (lvt_should_mask(lvt)) {
                x2apic_write(LAPIC_REG_LVT_CMCI, lvt | (1 << 16));
            }
        }
        lvt = x2apic_read(LAPIC_REG_LVT_TIMER);
        if (lvt_should_mask(lvt)) {
            x2apic_write(LAPIC_REG_LVT_TIMER, lvt | (1 << 16));
        }
        if (max_lvt >= 5) {
            lvt = x2apic_read(LAPIC_REG_LVT_THERMAL);
            if (lvt_should_mask(lvt)) {
                x2apic_write(LAPIC_REG_LVT_THERMAL, lvt | (1 << 16));
            }
        }
        if (max_lvt >= 4) {
            lvt = x2apic_read(LAPIC_REG_LVT_PMC);
            if (lvt_should_mask(lvt)) {
                x2apic_write(LAPIC_REG_LVT_PMC, lvt | (1 << 16));
            }
        }
        lvt = x2apic_read(LAPIC_REG_LVT_ERROR);
        if (lvt_should_mask(lvt)) {
            x2apic_write(LAPIC_REG_LVT_ERROR, lvt | (1 << 16));
        }
        lvt = x2apic_read(LAPIC_REG_LVT_LINT0);
        if (lvt_should_mask(lvt)) {
            x2apic_write(LAPIC_REG_LVT_LINT0, pending_lint0 != UINT32_MAX ? pending_lint0 : lvt | (1 << 16));
        }
        lvt = x2apic_read(LAPIC_REG_LVT_LINT1);
        if (lvt_should_mask(lvt)) {
            x2apic_write(LAPIC_REG_LVT_LINT1, pending_lint1 != UINT32_MAX ? pending_lint1 : lvt | (1 << 16));
        }
    } else {
        lapic_write(LAPIC_REG_SVR, 0x1ff);
        lapic_write(LAPIC_REG_TPR, 0);
        if (max_lvt >= 6) {
            lvt = lapic_read(LAPIC_REG_LVT_CMCI);
            if (lvt_should_mask(lvt)) {
                lapic_write(LAPIC_REG_LVT_CMCI, lvt | (1 << 16));
            }
        }
        lvt = lapic_read(LAPIC_REG_LVT_TIMER);
        if (lvt_should_mask(lvt)) {
            lapic_write(LAPIC_REG_LVT_TIMER, lvt | (1 << 16));
        }
        if (max_lvt >= 5) {
            lvt = lapic_read(LAPIC_REG_LVT_THERMAL);
            if (lvt_should_mask(lvt)) {
                lapic_write(LAPIC_REG_LVT_THERMAL, lvt | (1 << 16));
            }
        }
        if (max_lvt >= 4) {
            lvt = lapic_read(LAPIC_REG_LVT_PMC);
            if (lvt_should_mask(lvt)) {
                lapic_write(LAPIC_REG_LVT_PMC, lvt | (1 << 16));
            }
        }
        lvt = lapic_read(LAPIC_REG_LVT_ERROR);
        if (lvt_should_mask(lvt)) {
            lapic_write(LAPIC_REG_LVT_ERROR, lvt | (1 << 16));
        }
        lvt = lapic_read(LAPIC_REG_LVT_LINT0);
        if (lvt_should_mask(lvt)) {
            lapic_write(LAPIC_REG_LVT_LINT0, pending_lint0 != UINT32_MAX ? pending_lint0 : lvt | (1 << 16));
        }
        lvt = lapic_read(LAPIC_REG_LVT_LINT1);
        if (lvt_should_mask(lvt)) {
            lapic_write(LAPIC_REG_LVT_LINT1, pending_lint1 != UINT32_MAX ? pending_lint1 : lvt | (1 << 16));
        }
    }
}

void lapic_configure_bsp(void) {
    struct madt *madt = acpi_get_table("APIC", 0);
    if (madt == NULL) {
        return;
    }

    // Detect x2APIC from MSR
    bool is_x2 = !!(rdmsr(0x1b) & (1 << 10));

    // Find the BSP entry by matching LAPIC ID
    uint32_t bsp_lapic_id;
    if (is_x2) {
        bsp_lapic_id = x2apic_read(LAPIC_REG_ID);
    } else {
        bsp_lapic_id = lapic_read(LAPIC_REG_ID) >> 24;
    }

    uint32_t bsp_acpi_uid = 0;

    for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
      (uintptr_t)madt_ptr + 1 < (uintptr_t)madt + madt->header.length;
      madt_ptr += *(madt_ptr + 1)) {
        if (*(madt_ptr + 1) == 0
         || (uintptr_t)madt_ptr + *(madt_ptr + 1) > (uintptr_t)madt + madt->header.length) {
            break;
        }
        switch (*madt_ptr) {
            case 0: {
                if (*(madt_ptr + 1) < sizeof(struct madt_lapic)) {
                    continue;
                }
                struct madt_lapic *lapic = (void *)madt_ptr;
                if (lapic->lapic_id == bsp_lapic_id) {
                    bsp_acpi_uid = lapic->acpi_processor_uid;
                    goto found;
                }
                continue;
            }
            case 9: {
                if (!is_x2) {
                    continue;
                }
                if (*(madt_ptr + 1) < sizeof(struct madt_x2apic)) {
                    continue;
                }
                struct madt_x2apic *x2lapic = (void *)madt_ptr;
                if (x2lapic->x2apic_id == bsp_lapic_id) {
                    bsp_acpi_uid = x2lapic->acpi_processor_uid;
                    goto found;
                }
                continue;
            }
        }
    }

found:
    lapic_prep_lint(madt, bsp_acpi_uid, is_x2);
    lapic_configure_handoff_state();
}

struct dmar {
    struct sdt header;
    uint8_t host_address_width;
    uint8_t flags;
    uint8_t reserved[10];
    symbol  remapping_structures;
} __attribute__((packed));

bool lapic_check(void) {
    uint32_t eax, ebx, ecx, edx;
    if (!cpuid(1, 0, &eax, &ebx, &ecx, &edx))
        return false;

    if (!(edx & (1 << 9)))
        return false;

    return true;
}

uint32_t lapic_read(uint32_t reg) {
    size_t lapic_mmio_base = (size_t)(rdmsr(0x1b) & 0xfffff000);
    return mmind(lapic_mmio_base + reg);
}

void lapic_write(uint32_t reg, uint32_t data) {
    size_t lapic_mmio_base = (size_t)(rdmsr(0x1b) & 0xfffff000);
    mmoutd(lapic_mmio_base + reg, data);
}

void lapic_icr_wait(void) {
    for (int i = 0; i < 1000000; i++) {
        if (!(lapic_read(LAPIC_REG_ICR0) & (1 << 12))) {
            return;
        }
        asm volatile ("pause");
    }
}

bool x2apic_check(void) {
    uint32_t eax, ebx, ecx, edx;
    if (!cpuid(1, 0, &eax, &ebx, &ecx, &edx))
        return false;

    if (!(ecx & (1 << 21)))
        return false;

    // According to the Intel VT-d spec, we're required
    // to check if bit 0 and 1 of the flags field of the
    // DMAR ACPI table are set, and if they are, we should
    // not report x2APIC capabilities.
    struct dmar *dmar = acpi_get_table("DMAR", 0);
    if (!dmar)
        return true;

    if ((dmar->flags & (1 << 0)) && (dmar->flags & (1 << 1)))
        return false;

    return true;
}

static bool x2apic_mode = false;

bool x2apic_enable(void) {
    if (!x2apic_check())
        return false;

    uint64_t ia32_apic_base = rdmsr(0x1b);
    ia32_apic_base |= (1 << 10);
    wrmsr(0x1b, ia32_apic_base);

    x2apic_mode = true;

    return true;
}

bool x2apic_disable(void) {
    uint64_t msr = rdmsr(0x1b);
    if (!(msr & (1 << 10)))
        return true;

    // Check for LEGACY_XAPIC_DISABLED (Intel Meteor Lake+).
    // CPUID.07H.0:EDX[29] enumerates IA32_ARCH_CAPABILITIES MSR (0x10A).
    // IA32_ARCH_CAPABILITIES bit 21 = XAPIC_DISABLE feature supported.
    // IA32_XAPIC_DISABLE_STATUS MSR (0xBD) bit 0 = xAPIC permanently disabled.
    uint32_t eax, ebx, ecx, edx;
    if (cpuid(7, 0, &eax, &ebx, &ecx, &edx) && (edx & (1 << 29))) {
        uint64_t arch_caps = rdmsr(0x10a);
        if (arch_caps & (1 << 21)) {
            if (rdmsr(0xbd) & 1) {
                return false;
            }
        }
    }

    // Save LAPIC state; clearing EN resets all registers except APIC ID.
    uint32_t max_lvt = (x2apic_read(LAPIC_REG_VERSION) >> 16) & 0xff;
    uint32_t saved_svr = x2apic_read(LAPIC_REG_SVR);
    uint32_t saved_tpr = x2apic_read(LAPIC_REG_TPR);
    uint32_t saved_timer = x2apic_read(LAPIC_REG_LVT_TIMER);
    uint32_t saved_lint0 = x2apic_read(LAPIC_REG_LVT_LINT0);
    uint32_t saved_lint1 = x2apic_read(LAPIC_REG_LVT_LINT1);
    uint32_t saved_error = x2apic_read(LAPIC_REG_LVT_ERROR);
    uint32_t saved_pmc = max_lvt >= 4 ? x2apic_read(LAPIC_REG_LVT_PMC) : 0;
    uint32_t saved_thermal = max_lvt >= 5 ? x2apic_read(LAPIC_REG_LVT_THERMAL) : 0;
    uint32_t saved_cmci = max_lvt >= 6 ? x2apic_read(LAPIC_REG_LVT_CMCI) : 0;

    // Transition x2APIC -> disabled -> xAPIC.
    // Direct x2APIC -> xAPIC is an invalid transition (#GP).
    msr &= ~((1ULL << 11) | (1ULL << 10));
    wrmsr(0x1b, msr);

    msr |= (1ULL << 11);
    wrmsr(0x1b, msr);

    x2apic_mode = false;

    // Restore LAPIC state. SVR is restored last to re-enable the APIC.
    lapic_write(LAPIC_REG_TPR, saved_tpr);
    lapic_write(LAPIC_REG_LVT_TIMER, saved_timer);
    lapic_write(LAPIC_REG_LVT_LINT0, saved_lint0);
    lapic_write(LAPIC_REG_LVT_LINT1, saved_lint1);
    lapic_write(LAPIC_REG_LVT_ERROR, saved_error);
    if (max_lvt >= 4) lapic_write(LAPIC_REG_LVT_PMC, saved_pmc);
    if (max_lvt >= 5) lapic_write(LAPIC_REG_LVT_THERMAL, saved_thermal);
    if (max_lvt >= 6) lapic_write(LAPIC_REG_LVT_CMCI, saved_cmci);
    lapic_write(LAPIC_REG_SVR, saved_svr);

    return true;
}

void lapic_eoi(void) {
    if (!x2apic_mode) {
        lapic_write(0xb0, 0);
    } else {
        x2apic_write(0xb0, 0);
    }
}

uint64_t x2apic_read(uint32_t reg) {
    return rdmsr(0x800 + (reg >> 4));
}

void x2apic_write(uint32_t reg, uint64_t data) {
    wrmsr(0x800 + (reg >> 4), data);
}

static struct madt_io_apic **io_apics = NULL;
static size_t max_io_apics = 0;

void init_io_apics(void) {
    static bool already_inited = false;
    if (already_inited) {
        return;
    }

    struct madt *madt = acpi_get_table("APIC", 0);

    if (madt == NULL) {
        goto out;
    }

    for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
      (uintptr_t)madt_ptr + 1 < (uintptr_t)madt + madt->header.length;
      madt_ptr += *(madt_ptr + 1)) {
        if (*(madt_ptr + 1) == 0
         || (uintptr_t)madt_ptr + *(madt_ptr + 1) > (uintptr_t)madt + madt->header.length) {
            break;
        }
        switch (*madt_ptr) {
            case 1: {
                if (*(madt_ptr + 1) < sizeof(struct madt_io_apic))
                    continue;
                max_io_apics++;
                continue;
            }
        }
    }

    io_apics = ext_mem_alloc_counted(max_io_apics, sizeof(struct madt_io_apic *));
    max_io_apics = 0;

    for (uint8_t *madt_ptr = (uint8_t *)madt->madt_entries_begin;
      (uintptr_t)madt_ptr + 1 < (uintptr_t)madt + madt->header.length;
      madt_ptr += *(madt_ptr + 1)) {
        if (*(madt_ptr + 1) == 0
         || (uintptr_t)madt_ptr + *(madt_ptr + 1) > (uintptr_t)madt + madt->header.length) {
            break;
        }
        switch (*madt_ptr) {
            case 1: {
                if (*(madt_ptr + 1) < sizeof(struct madt_io_apic))
                    continue;
                io_apics[max_io_apics++] = (void *)madt_ptr;
                continue;
            }
        }
    }

out:
    already_inited = true;
}

uint32_t io_apic_read(size_t io_apic, uint32_t reg) {
    uintptr_t base = (uintptr_t)io_apics[io_apic]->address;
    mmoutd(base, reg);
    return mmind(base + 16);
}

void io_apic_write(size_t io_apic, uint32_t reg, uint32_t value) {
    uintptr_t base = (uintptr_t)io_apics[io_apic]->address;
    mmoutd(base, reg);
    mmoutd(base + 16, value);
}

uint32_t io_apic_gsi_count(size_t io_apic) {
    return ((io_apic_read(io_apic, 1) & 0xff0000) >> 16) + 1;
}

void io_apic_mask_all(bool mask_nmi_and_extint) {
    for (size_t i = 0; i < max_io_apics; i++) {
        uint32_t gsi_count = io_apic_gsi_count(i);
        for (uint32_t j = 0; j < gsi_count; j++) {
            uintptr_t ioredtbl = j * 2 + 16;
            switch ((io_apic_read(i, ioredtbl) >> 8) & 0b111) {
                case 0b000: // Fixed
                case 0b001: // Lowest Priority
                    break;
                case 0b100: // NMI
                case 0b111: // ExtINT
                    if (!mask_nmi_and_extint) {
                        continue;
                    }
                    break;
                default:
                    continue;
            }

            io_apic_write(i, ioredtbl, io_apic_read(i, ioredtbl) | (1 << 16));
        }
    }
}

#endif
