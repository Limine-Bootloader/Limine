// RISC-V Image header (Linux Documentation/arch/riscv/boot-image-header.rst),
// then an entry that records a0-a3, satp, sstatus and sie.

#ifndef IMAGE_VERSION
#define IMAGE_VERSION 0x2
#endif

.option norvc
.section .head, "ax"
.globl _start
_start:
    j entry
    .long 0
    .quad 0x200000
    .quad _image_size
    .quad 0
    .long IMAGE_VERSION
    .long 0
    .quad 0
    .quad 0x5643534952
    .long 0x05435352
    .long 0

entry:
    lla t0, stub_regs
    sd a0, 0(t0)
    sd a1, 8(t0)
    sd a2, 16(t0)
    sd a3, 24(t0)
    csrr t1, satp
    sd t1, 32(t0)
    csrr t1, sstatus
    sd t1, 40(t0)
    csrr t1, sie
    sd t1, 48(t0)
    lla t1, _start
    sd t1, 72(t0)
    lla sp, stack_top
    call stub_main
1:
    wfi
    j 1b
