// LoongArch kernel header (Linux Documentation/arch/loongarch/booting.rst),
// then an entry that records a0-a3, CRMD and the direct mapping windows.

.section .head, "ax"
.globl _start
_start:
    .long 0x5a4d
    .long 0
    .quad LOAD_OFFSET + (entry - _start)
    .quad _image_size
    .quad LOAD_OFFSET
    .quad 0
    .quad 0
    .quad 0
    .long 0x818223cd
    .long 0

.globl entry
entry:
    la.pcrel $t0, stub_regs
    st.d $a0, $t0, 0
    st.d $a1, $t0, 8
    st.d $a2, $t0, 16
    st.d $a3, $t0, 24
    csrrd $t1, 0x0
    st.d $t1, $t0, 32
    csrrd $t1, 0x180
    st.d $t1, $t0, 40
    csrrd $t1, 0x181
    st.d $t1, $t0, 48
    csrrd $t1, 0x182
    st.d $t1, $t0, 56
    csrrd $t1, 0x183
    st.d $t1, $t0, 64
    la.pcrel $t1, _start
    st.d $t1, $t0, 72
    la.pcrel $sp, stack_top
    bl stub_main
1:
    idle 0
    b 1b
