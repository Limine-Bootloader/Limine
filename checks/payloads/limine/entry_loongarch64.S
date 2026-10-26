#include "state.h"

// Takes the snapshot into the slots $t0 points to. The original $t0 and $t1
// are at 0($sp) and 8($sp); $sp is 16 below its value at entry.
.macro CAPTURE
    st.d $r1, $t0, 8
    st.d $r2, $t0, 16
    st.d $r4, $t0, 32
    st.d $r5, $t0, 40
    st.d $r6, $t0, 48
    st.d $r7, $t0, 56
    st.d $r8, $t0, 64
    st.d $r9, $t0, 72
    st.d $r10, $t0, 80
    st.d $r11, $t0, 88
    st.d $r14, $t0, 112
    st.d $r15, $t0, 120
    st.d $r16, $t0, 128
    st.d $r17, $t0, 136
    st.d $r18, $t0, 144
    st.d $r19, $t0, 152
    st.d $r20, $t0, 160
    st.d $r21, $t0, 168
    st.d $r22, $t0, 176
    st.d $r23, $t0, 184
    st.d $r24, $t0, 192
    st.d $r25, $t0, 200
    st.d $r26, $t0, 208
    st.d $r27, $t0, 216
    st.d $r28, $t0, 224
    st.d $r29, $t0, 232
    st.d $r30, $t0, 240
    st.d $r31, $t0, 248
    ld.d $t1, $sp, 0
    st.d $t1, $t0, 96
    ld.d $t1, $sp, 8
    st.d $t1, $t0, 104
    addi.d $sp, $sp, 16
    st.d $sp, $t0, 24
    csrrd $t1, 0x0
    st.d $t1, $t0, ST_CRMD * 8
    csrrd $t1, 0x1
    st.d $t1, $t0, ST_PRMD * 8
    csrrd $t1, 0x2
    st.d $t1, $t0, ST_EUEN * 8
    csrrd $t1, 0x4
    st.d $t1, $t0, ST_ECFG * 8
    csrrd $t1, 0xc
    st.d $t1, $t0, ST_EENTRY * 8
    csrrd $t1, 0x93
    st.d $t1, $t0, ST_MERRENTRY * 8
    csrrd $t1, 0x88
    st.d $t1, $t0, ST_TLBRENTRY * 8
    csrrd $t1, 0x19
    st.d $t1, $t0, ST_PGDL * 8
    csrrd $t1, 0x1a
    st.d $t1, $t0, ST_PGDH * 8
    csrrd $t1, 0x1c
    st.d $t1, $t0, ST_PWCL * 8
    csrrd $t1, 0x1d
    st.d $t1, $t0, ST_PWCH * 8
    csrrd $t1, 0x1e
    st.d $t1, $t0, ST_STLBPS * 8
    csrrd $t1, 0x180
    st.d $t1, $t0, ST_DMW0 * 8
    csrrd $t1, 0x181
    st.d $t1, $t0, ST_DMW1 * 8
    csrrd $t1, 0x182
    st.d $t1, $t0, ST_DMW2 * 8
    csrrd $t1, 0x183
    st.d $t1, $t0, ST_DMW3 * 8
    csrrd $t1, 0x20
    st.d $t1, $t0, ST_CPUID * 8
    csrrd $t1, 0x3
    st.d $t1, $t0, ST_MISC * 8
.endm

.section .text

.globl _start
_start:
    addi.d $sp, $sp, -16
    st.d $t0, $sp, 0
    st.d $t1, $sp, 8
    la.pcrel $t0, bsp_state
    CAPTURE
    li.d $t1, ENTRY_ELF
    la.pcrel $t0, entry_id
    st.d $t1, $t0, 0
    bl kmain
1:
    idle 0
    b 1b

.globl entry_req
entry_req:
    addi.d $sp, $sp, -16
    st.d $t0, $sp, 0
    st.d $t1, $sp, 8
    la.pcrel $t0, bsp_state
    CAPTURE
    li.d $t1, ENTRY_REQUEST
    la.pcrel $t0, entry_id
    st.d $t1, $t0, 0
    bl kmain
1:
    idle 0
    b 1b

// $a0 is the processor's limine_mp_info, whose extra_argument (offset 32) is
// its slot.
.globl ap_entry
ap_entry:
    addi.d $sp, $sp, -16
    st.d $t0, $sp, 0
    st.d $t1, $sp, 8
    ld.d $t0, $a0, 32
    li.d $t1, ST_COUNT * 8
    mul.d $t0, $t0, $t1
    la.pcrel $t1, ap_states
    add.d $t0, $t0, $t1
    CAPTURE
    bl ap_main
1:
    idle 0
    b 1b
