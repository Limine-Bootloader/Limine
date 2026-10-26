; The Multiboot 1 header and entry. FLAGS, and with bit 2 MODE_TYPE, WIDTH,
; HEIGHT and DEPTH, come in as defines. AOUT fills in the address fields, for
; the flat binary build; AOUT_WHOLE_FILE leaves load_end_addr 0.

bits 32

extern mb1_main
extern __load_start, __load_end, __bss_end
global _start
global mb_regs, mb_gdtr, mb_idtr

section .multiboot
align 4
mb_header:
    dd 0x1badb002
    dd FLAGS
    dd -(0x1badb002 + FLAGS)
%if FLAGS & 0x10004
%ifdef AOUT
    dd mb_header
    dd __load_start
%ifdef AOUT_WHOLE_FILE
    dd 0
%else
    dd __load_end
%endif
    dd __bss_end
    dd _start
%else
    times 5 dd 0
%endif
%endif
%if FLAGS & 4
    dd MODE_TYPE
    dd WIDTH
    dd HEIGHT
    dd DEPTH
%endif

section .text
_start:
    ; No stack is promised, so registers go straight to memory first.
    mov [mb_regs + 0 * 4], eax
    mov [mb_regs + 1 * 4], ebx
    mov [mb_regs + 2 * 4], esp
    mov esp, stack_top
    pushfd
    pop eax
    mov [mb_regs + 3 * 4], eax
    xor eax, eax
    mov ax, cs
    mov [mb_regs + 4 * 4], eax
    mov ax, ds
    mov [mb_regs + 5 * 4], eax
    mov ax, es
    mov [mb_regs + 6 * 4], eax
    mov ax, fs
    mov [mb_regs + 7 * 4], eax
    mov ax, gs
    mov [mb_regs + 8 * 4], eax
    mov ax, ss
    mov [mb_regs + 9 * 4], eax
    mov eax, cr0
    mov [mb_regs + 10 * 4], eax
    sgdt [mb_gdtr]
    sidt [mb_idtr]
    call mb1_main
.halt:
    cli
    hlt
    jmp .halt

section .data
align 4
mb_regs:
    times 16 dd 0
mb_gdtr:
    times 8 db 0
mb_idtr:
    times 8 db 0

section .bss
align 16
    resb 16384
stack_top:
