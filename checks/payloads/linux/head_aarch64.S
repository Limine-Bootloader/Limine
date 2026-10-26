// arm64 Image header (Linux Documentation/arch/arm64/booting.rst), then an
// entry that records x0-x3, CurrentEL, SCTLR and DAIF before anything else.

.section .head, "ax"
.globl _start
_start:
    b entry
    .long 0
    .quad 0
    .quad _image_size
    .quad IMAGE_FLAGS
    .quad 0
    .quad 0
    .quad 0
    .long 0x644d5241
    .long 0

entry:
    adrp x9, stub_regs
    add x9, x9, :lo12:stub_regs
    stp x0, x1, [x9]
    stp x2, x3, [x9, #16]
    mrs x10, CurrentEL
    str x10, [x9, #32]
    cmp x10, #(2 << 2)
    b.eq 1f
    mrs x11, sctlr_el1
    b 2f
1:
    mrs x11, sctlr_el2
2:
    str x11, [x9, #40]
    mrs x11, daif
    str x11, [x9, #48]
    adr x11, _start
    str x11, [x9, #72]
    adrp x11, stack_top
    add x11, x11, :lo12:stack_top
    mov sp, x11
    bl stub_main
3:
    wfi
    b 3b
