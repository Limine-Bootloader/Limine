#include "state.h"

// Takes the snapshot into the slots t1 points to. The original t1 and t2 are
// at 0(sp) and 8(sp); sp is 16 below its value at entry.
.macro CAPTURE
    sd x1, 8(t1)
    sd x3, 24(t1)
    sd x4, 32(t1)
    sd x5, 40(t1)
    sd x8, 64(t1)
    sd x9, 72(t1)
    sd x10, 80(t1)
    sd x11, 88(t1)
    sd x12, 96(t1)
    sd x13, 104(t1)
    sd x14, 112(t1)
    sd x15, 120(t1)
    sd x16, 128(t1)
    sd x17, 136(t1)
    sd x18, 144(t1)
    sd x19, 152(t1)
    sd x20, 160(t1)
    sd x21, 168(t1)
    sd x22, 176(t1)
    sd x23, 184(t1)
    sd x24, 192(t1)
    sd x25, 200(t1)
    sd x26, 208(t1)
    sd x27, 216(t1)
    sd x28, 224(t1)
    sd x29, 232(t1)
    sd x30, 240(t1)
    sd x31, 248(t1)
    ld t2, 0(sp)
    sd t2, 48(t1)
    ld t2, 8(sp)
    sd t2, 56(t1)
    addi sp, sp, 16
    sd sp, 16(t1)
    csrr t2, sstatus
    sd t2, (ST_SSTATUS * 8)(t1)
    csrr t2, sie
    sd t2, (ST_SIE * 8)(t1)
    csrr t2, stvec
    sd t2, (ST_STVEC * 8)(t1)
    csrr t2, satp
    sd t2, (ST_SATP * 8)(t1)
    csrr t2, sip
    sd t2, (ST_SIP * 8)(t1)
.endm

.section .text

.globl _start
_start:
    addi sp, sp, -16
    sd t1, 0(sp)
    sd t2, 8(sp)
    lla t1, bsp_state
    CAPTURE
    li t2, ENTRY_ELF
    lla t1, entry_id
    sd t2, 0(t1)
    call kmain
1:
    wfi
    j 1b

.globl entry_req
entry_req:
    addi sp, sp, -16
    sd t1, 0(sp)
    sd t2, 8(sp)
    lla t1, bsp_state
    CAPTURE
    li t2, ENTRY_REQUEST
    lla t1, entry_id
    sd t2, 0(t1)
    call kmain
1:
    wfi
    j 1b

// a0 is the processor's limine_mp_info; extra_argument (offset 32) is its slot.
.globl ap_entry
ap_entry:
    addi sp, sp, -16
    sd t1, 0(sp)
    sd t2, 8(sp)
    ld t1, 32(a0)
    li t2, ST_COUNT * 8
    mul t1, t1, t2
    lla t2, ap_states
    add t1, t1, t2
    CAPTURE
    call ap_main
1:
    wfi
    j 1b
