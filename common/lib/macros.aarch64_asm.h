// Branch to \el1 if in EL1, or to \el2 if in EL2
// Uses \reg, halts if not in EL1 or EL2
.macro PICK_EL reg, el1, el2
    mrs \reg, currentel
    and \reg, \reg, #0b1100

    cmp \reg, #0b0100 // EL1?
    b.eq \el1
    cmp \reg, #0b1000 // EL2?
    b.eq \el2

    // Halt otherwise
    msr daifset, #0b1111
99:
    wfi
    b 99b
.endm


// Zero out all general purpose registers apart from X0
.macro ZERO_REGS_EXCEPT_X0
    mov x1, xzr
    mov x2, xzr
    mov x3, xzr
    mov x4, xzr
    mov x5, xzr
    mov x6, xzr
    mov x7, xzr
    mov x8, xzr
    mov x9, xzr
    mov x10, xzr
    mov x11, xzr
    mov x12, xzr
    mov x13, xzr
    mov x14, xzr
    mov x15, xzr
    mov x16, xzr
    mov x17, xzr
    mov x18, xzr
    mov x19, xzr
    mov x20, xzr
    mov x21, xzr
    mov x22, xzr
    mov x23, xzr
    mov x24, xzr
    mov x25, xzr
    mov x26, xzr
    mov x27, xzr
    mov x28, xzr
    mov x29, xzr
    mov x30, xzr
.endm

// Set EL2 up so that EL1, once entered by ERET, runs as it would on a PE
// without EL2, for the Armv8.0 PEs that can_drop_to_el1() accepts
// Uses \tmp1 and \tmp2
.macro INIT_EL2_FOR_EL1 tmp1, tmp2
    // Most fields below reset to UNKNOWN values and firmware may have changed
    // the rest, so every register is written whole: a stray trap or routing
    // bit would survive a read-modify-write. Sections are those of the
    // Arm ARM, DDI 0487M.c, unless stated otherwise.

    // HCR_EL2 (D24.2.61) starts as RW alone: EL1 is AArch64, nothing traps
    // or routes to EL2, no virtual interrupt is pending, and there is no
    // stage 2.
    mov \tmp2, #(1 << 31)

    // HVC is UNDEFINED without EL2 (C6.2.175). HCD can only make it so where
    // EL3 is absent, being RES0 otherwise.
    mrs \tmp1, id_aa64pfr0_el1
    ubfx \tmp1, \tmp1, #12, #4 // EL3
    cbnz \tmp1, .L_el2_hcd_done\@
    orr \tmp2, \tmp2, #(1 << 29)
.L_el2_hcd_done\@:

    // FEAT_CSV2_2 and FEAT_CSV2_1p2 are permitted in Armv8.0 (A2.2.1) and
    // give EL1 the SCXTNUM registers, which EnSCXT traps while clear. It is
    // RES0 without them.
    mrs \tmp1, id_aa64pfr0_el1
    ubfx \tmp1, \tmp1, #57, #3 // CSV2 >= 0b0010
    cbnz \tmp1, .L_el2_scxt\@
    mrs \tmp1, id_aa64pfr1_el1
    ubfx \tmp1, \tmp1, #33, #3 // CSV2_frac >= 0b0010
    cbz \tmp1, .L_el2_scxt_done\@
.L_el2_scxt\@:
    orr \tmp2, \tmp2, #(1 << 53)
.L_el2_scxt_done\@:
    msr hcr_el2, \tmp2

    // CPTR_EL2 (D24.2.37): the RES1 bits alone, TZ and TSM among them as SVE
    // and SME are absent, so FP, trace, and CPACR_EL1 accesses do not trap.
    mov \tmp1, #0x33ff
    msr cptr_el2, \tmp1

    // HSTR_EL2 (D24.2.76) holds nothing but traps of AArch32 CP15 accesses.
    msr hstr_el2, xzr

    // MDCR_EL2 (D24.3.17): HPMN set to the PMCR_EL0.N that EL2 reads (D24.5.8)
    // gives every event counter to EL1 and EL0, and the rest clear routes
    // debug exceptions to EL1 and traps nothing. Without PMUv3, which PMUVer
    // 0b0000 and 0b1111 both indicate (D24.2.79), PMCR_EL0 is UNDEFINED and
    // HPMN RES0.
    mov \tmp2, xzr
    mrs \tmp1, id_aa64dfr0_el1
    ubfx \tmp1, \tmp1, #8, #4 // PMUVer
    cbz \tmp1, .L_el2_pmu_done\@
    eor \tmp1, \tmp1, #0xf
    cbz \tmp1, .L_el2_pmu_done\@
    mrs \tmp2, pmcr_el0
    ubfx \tmp2, \tmp2, #11, #5
.L_el2_pmu_done\@:
    msr mdcr_el2, \tmp2

    // CNTHCTL_EL2 (D24.10.2): EL1PCTEN and EL1PCEN set, which is how they
    // behave without EL2, and EL2's event stream, which acts at every
    // Exception level, off. A zero offset makes the virtual counter the
    // physical one (D12.2.2).
    mov \tmp1, #3
    msr cnthctl_el2, \tmp1
    msr cntvoff_el2, xzr

    // EL2's own timer has no counterpart without EL2, and as its ENABLE
    // resets UNKNOWN, its interrupt could otherwise reach EL1 (D24.10.6).
    msr cnthp_ctl_el2, xzr

    // EL1&0 TLB entries carry the VMID even with stage 2 off (D8.16.3.1), and
    // VTTBR_EL2 has no reset value. Zero is VMID 0 at either width that
    // VTCR_EL2.VS allows.
    msr vttbr_el2, xzr

    // EL1 reads of MIDR_EL1 and MPIDR_EL1 return these, while EL2 reads the
    // PE's own values (D24.2.136, D24.2.137).
    mrs \tmp1, midr_el1
    msr vpidr_el2, \tmp1
    mrs \tmp1, mpidr_el1
    msr vmpidr_el2, \tmp1

    // HACR_EL2 and ACTLR_EL2 are IMPLEMENTATION DEFINED throughout (D24.2.59,
    // D24.2.3), so the architecture gives no value to write to them.

    // The GICv3 System register interface, per GIC IHI 0069H.b. In
    // ICC_SRE_EL2, Enable and SRE let EL1 reach and enable its own interface
    // (12.2.23, 12.2.24), and DIB and DFB disable bypass, which must be off
    // before EL1 enables an interrupt group but which EL1 cannot change with
    // EL2 present (3.2, 12.2.23).
    mrs \tmp1, id_aa64pfr0_el1
    ubfx \tmp1, \tmp1, #24, #4 // GIC
    cbz \tmp1, .L_el2_gic_done\@
    mov \tmp1, #0xf
    msr icc_sre_el2, \tmp1

    // ICH_HCR_EL2 traps unless the SRE just written is in effect (12.4.5,
    // D24.1.2.2), and SRE is RAZ/WI where EL3 withholds the interface
    // (12.2.24).
    isb
    mrs \tmp1, icc_sre_el2
    tbz \tmp1, #0, .L_el2_gic_done\@

    // ICH_HCR_EL2 (12.4.5): the virtual CPU interface off, and no trap of
    // EL1's ICC_* accesses.
    msr ich_hcr_el2, xzr
.L_el2_gic_done\@:

    // The caller's TLB maintenance reads the VMID, and its ERET reads
    // HCR_EL2.{TGE, RW} before synchronising (D24.1.2.2, D1.4.4.2).
    isb
.endm
