; The protected-mode payload: the 32-bit entry at its start, the 64-bit one
; 0x200 in, as the x86 boot protocol places them, then the two builds.

%define PART_A_OFFSET 0x400
%define PART_B_OFFSET 0x8000

bits 32
entry32:
    jmp part_a
    times 0x200 - ($ - $$) db 0
bits 64
entry64:
    jmp part_b
    times PART_A_OFFSET - ($ - $$) db 0
part_a:
    incbin "part_a.bin"
%if ($ - $$) > PART_B_OFFSET
%error "the 32-bit part runs into the 64-bit one"
%endif
    times PART_B_OFFSET - ($ - $$) db 0
part_b:
    incbin "part_b.bin"
