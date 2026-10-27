/* The Multiboot check kernels, at 3MiB: physical, with bit 20 set for the A20
   check, and clear of what firmware keeps low. */

ENTRY(_start)

/* The segments every linker is to make, split where the access changes. */
PHDRS
{
    header PT_LOAD FLAGS(0x04);
    text PT_LOAD FLAGS(0x05);
    rodata PT_LOAD FLAGS(0x04);
    data PT_LOAD FLAGS(0x06);
}

SECTIONS
{
    . = 0x300000;
    __load_start = .;
    .multiboot : { KEEP(*(.multiboot)) } :header
    .text : { *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :rodata
    .data : { *(.data .data.*) } :data
    __load_end = .;
    .bss : { *(.bss .bss.*) *(COMMON) } :data
    __bss_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }
}
