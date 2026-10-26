#if defined (__x86_64__) || defined (__i386__)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/iommu.h>
#include <sys/cpu.h>
#include <lib/acpi.h>
#include <lib/libc.h>

// Intel VT-d registers
#define VTD_GCMD_REG  0x18
#define VTD_GSTS_REG  0x1C
#define VTD_FSTS_REG  0x34
#define VTD_IQT_REG   0x88
#define VTD_IQA_REG   0x90

// GSTS/GCMD bit positions
#define VTD_GSTS_TES  (1u << 31)  // Translation Enable Status
#define VTD_GSTS_QIES (1u << 26)  // Queued Invalidation Enable Status
#define VTD_GSTS_IRES (1u << 25)  // Interrupt Remapping Enable Status

// FSTS bits that stop the invalidation queue
#define VTD_FSTS_IQE  (1u << 4)   // Invalidation Queue Error
#define VTD_FSTS_ICE  (1u << 5)   // Invalidation Completion Error
#define VTD_FSTS_ITE  (1u << 6)   // Invalidation Time-out Error

// Invalidation Wait Descriptor with Status Write, its status data 1
#define VTD_INV_WAIT_DSC ((1ull << 32) | (1u << 5) | 0x5)

// Mask to clear one-shot command bits when reading GSTS for use as GCMD base.
// One-shot bits auto-clear after hardware processes them and must not be
// carried over from GSTS into GCMD writes:
//   Bit 30: SRTP (Set Root Table Pointer)
//   Bit 29: SFL  (Set Fault Log)
//   Bit 27: WBF  (Write Buffer Flush)
//   Bit 24: SIRTP (Set Interrupt Remap Table Pointer)
// All other bits (TE, EAFL, QIE, IRE, CFI) are persistent toggles.
#define VTD_GCMD_ONESHOT_MASK 0x96FFFFFF

static volatile uint32_t vtd_wait_status;

// Queued invalidation may only be disabled once its queue is quiesced: empty,
// with an Invalidation Wait the last descriptor completed (Intel VT-d 5.20
// section 6.5.2).
static bool vtd_quiesce_queue(uintptr_t reg_base) {
    uint64_t iqa = mmind(reg_base + VTD_IQA_REG) | ((uint64_t)mmind(reg_base + VTD_IQA_REG + 4) << 32);
    uint64_t queue = iqa & ~(uint64_t)0xfff;
    if (queue > UINTPTR_MAX) {
        return false;
    }
    uint32_t dsc_size = (iqa & (1u << 11)) ? 32 : 16;
    uint32_t queue_size = (uint32_t)4096 << (iqa & 7);
    uint32_t tail = mmind(reg_base + VTD_IQT_REG) & 0x7fff0;

    volatile uint64_t *dsc = (volatile uint64_t *)(uintptr_t)(queue + tail);
    vtd_wait_status = 0;
    dsc[0] = VTD_INV_WAIT_DSC;
    dsc[1] = (uintptr_t)&vtd_wait_status;
    if (dsc_size == 32) {
        dsc[2] = 0;
        dsc[3] = 0;
    }
    mmoutd(reg_base + VTD_IQT_REG, (tail + dsc_size) & (queue_size - 1));

    while (vtd_wait_status != 1) {
        if (mmind(reg_base + VTD_FSTS_REG) & (VTD_FSTS_IQE | VTD_FSTS_ICE | VTD_FSTS_ITE)) {
            return false;
        }
        asm volatile ("pause");
    }
    return true;
}

static void vtd_disable_unit(uintptr_t reg_base) {
    uint32_t sts = mmind(reg_base + VTD_GSTS_REG);

    // Disable DMA translation first (most urgent: prevents stale lookups)
    if (sts & VTD_GSTS_TES) {
        uint32_t gcmd = (sts & VTD_GCMD_ONESHOT_MASK) & ~VTD_GSTS_TES;
        mmoutd(reg_base + VTD_GCMD_REG, gcmd);

        while ((sts = mmind(reg_base + VTD_GSTS_REG)) & VTD_GSTS_TES) {
            asm volatile ("pause");
        }
    }

    // Disable interrupt remapping (depends on QIE, so disable before QIE)
    if (sts & VTD_GSTS_IRES) {
        uint32_t gcmd = (sts & VTD_GCMD_ONESHOT_MASK) & ~VTD_GSTS_IRES;
        mmoutd(reg_base + VTD_GCMD_REG, gcmd);

        while ((sts = mmind(reg_base + VTD_GSTS_REG)) & VTD_GSTS_IRES) {
            asm volatile ("pause");
        }
    }

    // Disable queued invalidation last (was prerequisite for IRE). A queue that
    // cannot be quiesced is left on, as nothing promises it off.
    if ((sts & VTD_GSTS_QIES) && vtd_quiesce_queue(reg_base)) {
        uint32_t gcmd = (sts & VTD_GCMD_ONESHOT_MASK) & ~VTD_GSTS_QIES;
        mmoutd(reg_base + VTD_GCMD_REG, gcmd);

        while ((sts = mmind(reg_base + VTD_GSTS_REG)) & VTD_GSTS_QIES) {
            asm volatile ("pause");
        }
    }
}

void vtd_disable_all(void) {
    struct sdt *dmar = acpi_get_table("DMAR", 0);
    if (dmar == NULL) {
        return;
    }

    // DMAR header is 48 bytes: 36 (SDT) + 1 (width) + 1 (flags) + 10 (reserved)
    uint8_t *ptr = (uint8_t *)dmar + 48;
    uint8_t *end = (uint8_t *)dmar + dmar->length;

    while (ptr + 4 <= end) {
        uint16_t type   = *(uint16_t *)(ptr + 0);
        uint16_t length = *(uint16_t *)(ptr + 2);

        if (length < 4 || ptr + length > end) {
            break;
        }

        // Type 0 = DRHD (DMA Remapping Hardware Unit Definition)
        if (type == 0 && length >= 16) {
            uint64_t reg_base;
            memcpy(&reg_base, ptr + 8, sizeof(reg_base));
            vtd_disable_unit((uintptr_t)reg_base);
        }

        ptr += length;
    }
}

// AMD IOMMU control register (64-bit at offset 0x18)
#define AMDVI_CONTROL_REG     0x18
#define AMDVI_CONTROL_EN      (1u << 0)
#define AMDVI_CONTROL_EVT_LOG (1u << 2)
#define AMDVI_CONTROL_EVT_INT (1u << 3)
#define AMDVI_CONTROL_CMDBUF  (1u << 12)
#define AMDVI_CONTROL_PPR_LOG (1u << 13)
#define AMDVI_CONTROL_PPR_INT (1u << 14)
#define AMDVI_CONTROL_PPR     (1u << 15)
#define AMDVI_CONTROL_GA_LOG  (1u << 28)
#define AMDVI_CONTROL_GA_INT  (1u << 29)

// AMD IOMMU status register (64-bit at offset 0x2020)
#define AMDVI_STATUS_REG         0x2020
#define AMDVI_STATUS_EVTLOG_RUN  (1u << 3)
#define AMDVI_STATUS_CMDBUF_RUN  (1u << 4)
#define AMDVI_STATUS_PPRLOG_RUN  (1u << 7)
#define AMDVI_STATUS_GALOG_RUN   (1u << 8)

static void amdvi_disable_unit(uintptr_t mmio_base) {
    // Read low 32 bits of the 64-bit control register
    uint32_t ctrl_lo = mmind(mmio_base + AMDVI_CONTROL_REG);

    if (!(ctrl_lo & AMDVI_CONTROL_EN)) {
        return; // IOMMU not enabled
    }

    // Disable command buffer, logs and their interrupts before the master
    // enable. Clearing IommuEn while sub-features are still live can leave
    // queued descriptors and in-flight DMA in an undefined state.
    ctrl_lo &= ~(AMDVI_CONTROL_CMDBUF |
                 AMDVI_CONTROL_EVT_LOG | AMDVI_CONTROL_EVT_INT |
                 AMDVI_CONTROL_GA_LOG  | AMDVI_CONTROL_GA_INT  |
                 AMDVI_CONTROL_PPR_LOG | AMDVI_CONTROL_PPR_INT |
                 AMDVI_CONTROL_PPR);
    mmoutd(mmio_base + AMDVI_CONTROL_REG, ctrl_lo);

    // The *Run bits are level-sensitive and only drop to 0 once the engine
    // has actually drained, so wait for them before continuing.
    const uint32_t run_mask = AMDVI_STATUS_CMDBUF_RUN | AMDVI_STATUS_EVTLOG_RUN |
                              AMDVI_STATUS_PPRLOG_RUN | AMDVI_STATUS_GALOG_RUN;
    while (mmind(mmio_base + AMDVI_STATUS_REG) & run_mask) {
        asm volatile ("pause");
    }

    ctrl_lo &= ~AMDVI_CONTROL_EN;
    mmoutd(mmio_base + AMDVI_CONTROL_REG, ctrl_lo);
}

void amdvi_disable_all(void) {
    struct sdt *ivrs = acpi_get_table("IVRS", 0);
    if (ivrs == NULL) {
        return;
    }

    // IVRS header is 48 bytes: 36 (SDT) + 4 (IVinfo) + 8 (reserved)
    uint8_t *ptr = (uint8_t *)ivrs + 48;
    uint8_t *end = (uint8_t *)ivrs + ivrs->length;

    while (ptr + 4 <= end) {
        uint8_t  type   = *(uint8_t *)(ptr + 0);
        uint16_t length = *(uint16_t *)(ptr + 2);

        if (length < 4 || ptr + length > end) {
            break;
        }

        // IVHD types: 0x10 (basic), 0x11 (extended), 0x40 (extended, newer)
        if ((type == 0x10 || type == 0x11 || type == 0x40) && length >= 16) {
            uint64_t mmio_base;
            memcpy(&mmio_base, ptr + 8, sizeof(mmio_base));
            amdvi_disable_unit((uintptr_t)mmio_base);
        }

        ptr += length;
    }
}

void iommu_disable_all(void) {
    vtd_disable_all();
    amdvi_disable_all();
}

#endif
