// These are hard to explain, but they are basically used to set the output section
// type, as there is no standard, reliable, or even existing way to do so within the
// linker script. This is mostly for ld.gold.

.section .text_type_marker,"ax",%progbits
.section .rodata_type_marker,"a",%progbits
.section .data_type_marker,"wa",%progbits
