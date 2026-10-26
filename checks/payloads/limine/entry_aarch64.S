#include "state.h"

// Takes the snapshot into the slots x0 points to; the original x0 and x1 are
// on the stack, which is popped, leaving sp as it was at entry.
.macro CAPTURE
    stp x2, x3, [x0, #(2 * 8)]
    stp x4, x5, [x0, #(4 * 8)]
    stp x6, x7, [x0, #(6 * 8)]
    stp x8, x9, [x0, #(8 * 8)]
    stp x10, x11, [x0, #(10 * 8)]
    stp x12, x13, [x0, #(12 * 8)]
    stp x14, x15, [x0, #(14 * 8)]
    stp x16, x17, [x0, #(16 * 8)]
    stp x18, x19, [x0, #(18 * 8)]
    stp x20, x21, [x0, #(20 * 8)]
    stp x22, x23, [x0, #(22 * 8)]
    stp x24, x25, [x0, #(24 * 8)]
    stp x26, x27, [x0, #(26 * 8)]
    stp x28, x29, [x0, #(28 * 8)]
    str x30, [x0, #(30 * 8)]
    ldp x2, x3, [sp], #16
    stp x2, x3, [x0, #(0 * 8)]
    mov x2, sp
    str x2, [x0, #(ST_SP * 8)]
    mrs x2, CurrentEL
    str x2, [x0, #(ST_CURRENTEL * 8)]
    mrs x2, DAIF
    str x2, [x0, #(ST_DAIF * 8)]
    mrs x2, SPSel
    str x2, [x0, #(ST_SPSEL * 8)]
    mrs x2, sctlr_el1
    str x2, [x0, #(ST_SCTLR * 8)]
    mrs x2, tcr_el1
    str x2, [x0, #(ST_TCR * 8)]
    mrs x2, mair_el1
    str x2, [x0, #(ST_MAIR * 8)]
    mrs x2, ttbr0_el1
    str x2, [x0, #(ST_TTBR0 * 8)]
    mrs x2, ttbr1_el1
    str x2, [x0, #(ST_TTBR1 * 8)]
    mrs x2, cpacr_el1
    str x2, [x0, #(ST_CPACR * 8)]
    mrs x2, vbar_el1
    str x2, [x0, #(ST_VBAR * 8)]
    mrs x2, CurrentEL
    cmp x2, #(2 << 2)
    b.ne 9f
    mrs x2, hcr_el2
    str x2, [x0, #(ST_HCR_EL2 * 8)]
    mrs x2, cptr_el2
    str x2, [x0, #(ST_CPTR_EL2 * 8)]
    mrs x2, cnthctl_el2
    str x2, [x0, #(ST_CNTHCTL_EL2 * 8)]
    mrs x2, hstr_el2
    str x2, [x0, #(ST_HSTR_EL2 * 8)]
9:
.endm

.section .text

.globl _start
_start:
    // NZCV first, before anything can touch the flags.
    stp x0, x1, [sp, #-16]!
    mrs x1, NZCV
    adrp x0, bsp_state
    add x0, x0, :lo12:bsp_state
    str x1, [x0, #(ST_NZCV * 8)]
    CAPTURE
    mov x2, #ENTRY_ELF
    adrp x3, entry_id
    str x2, [x3, #:lo12:entry_id]
    bl kmain
1:
    wfi
    b 1b

.globl entry_req
entry_req:
    stp x0, x1, [sp, #-16]!
    mrs x1, NZCV
    adrp x0, bsp_state
    add x0, x0, :lo12:bsp_state
    str x1, [x0, #(ST_NZCV * 8)]
    CAPTURE
    mov x2, #ENTRY_REQUEST
    adrp x3, entry_id
    str x2, [x3, #:lo12:entry_id]
    bl kmain
1:
    wfi
    b 1b

// x0 is the processor's limine_mp_info, whose extra_argument (offset 32) the
// BSP set to its slot.
.globl ap_entry
ap_entry:
    stp x0, x1, [sp, #-16]!
    ldr x1, [x0, #32]
    mov x0, #(ST_COUNT * 8)
    mul x1, x1, x0
    adrp x0, ap_states
    add x0, x0, :lo12:ap_states
    add x0, x0, x1
    CAPTURE
    ldr x0, [x0, #(0 * 8)]
    bl ap_main
1:
    wfi
    b 1b
