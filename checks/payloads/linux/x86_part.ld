/* Both parts are position independent, so link them at 0. The tables -pie
   makes come before .bss, which the flat binary must not contain. */

SECTIONS
{
    . = PART_BASE;
    .text : {
        *(.entry)
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .got : { *(.got .got.*) }
    .dynamic : { *(.dynamic) }
    .bss : { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
