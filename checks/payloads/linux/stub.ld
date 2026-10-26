SECTIONS
{
    . = 0;
    .head : { KEEP(*(.head)) }
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) *(.srodata .srodata.*) }
    .data : { *(.data .data.*) *(.sdata .sdata.*) }
    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        __bss_end = .;
    }
    .stack (NOLOAD) : {
        . = ALIGN(16);
        . += 65536;
        stack_top = .;
    }
    _end = .;
    _image_size = _end;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
        *(.riscv.attributes)
        *(.ARM.attributes)
    }
}
