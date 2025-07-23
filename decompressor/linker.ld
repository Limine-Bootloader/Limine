OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

PHDRS
{
    text  PT_LOAD    FLAGS((1 << 0) | (1 << 2)) ;
    rodata PT_LOAD   FLAGS((1 << 2)) ;
    data  PT_LOAD    FLAGS((1 << 1) | (1 << 2)) ;
}

SECTIONS
{
    . = 0x70000;

    .text : {
        *(.entry)
        *(.text .text.*)
    } :text

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        bss_begin = .;
        *(.bss .bss.*)
        *(COMMON)
        bss_end = .;
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }
}
