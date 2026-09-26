PHDRS
{
    headers PT_PHDR PHDRS FLAGS(0x04);
    text    PT_LOAD FILEHDR PHDRS FLAGS(0x05);
    rodata  PT_LOAD FLAGS(0x04);
    data    PT_LOAD FLAGS(0x06);
    dynamic PT_DYNAMIC FLAGS(0x06);
}

SECTIONS
{
    . = SIZEOF_HEADERS;
    executable_start = . - SIZEOF_HEADERS;

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        *(.sdata2 .sdata2.*)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)

        KEEP(*(.limine_requests_start_marker))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end_marker))
    } :data

    .dynamic : {
        *(.dynamic)
    } :data :dynamic

    .bss : {
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.interp)
        *(.ARM.attributes)
        *(.riscv.attributes)
    }
}
