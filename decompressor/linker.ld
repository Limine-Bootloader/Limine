OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

PHDRS
{
    text  PT_LOAD    FLAGS(0x05);
    rodata PT_LOAD   FLAGS(0x04);
    data  PT_LOAD    FLAGS(0x06);
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
