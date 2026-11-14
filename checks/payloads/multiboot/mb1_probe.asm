; A Multiboot 1 kernel of position independent code, which checks can load
; anywhere: it writes the command line its information structure gives to the
; debug console, ends as the other check kernels do, and halts.

bits 32

%macro putc 1
    mov al, %1
    out 0xe9, al
%endmacro

section .multiboot
align 4
    dd 0x1badb002
    dd 0
    dd -0x1badb002

section .text
global _start
_start:
    ; Flags bit 2 means there is a command line, its address at offset 16
    ; (Multiboot 0.6.96 section 3.3).
    test dword [ebx], 4
    jz .done
    mov esi, [ebx + 16]
    mov ecx, 256
    cld
.next:
    lodsb
    test al, al
    jz .done
    out 0xe9, al
    loop .next
.done:
    putc 10
    putc 'E'
    putc ' '
    putc 'D'
    putc 'O'
    putc 'N'
    putc 'E'
    putc 10
.halt:
    cli
    hlt
    jmp .halt
