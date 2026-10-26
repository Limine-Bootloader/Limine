; The 64-bit boot protocol's entry: long mode, %rsi the zero page. Position
; independent, as a kernel taking this entry may be loaded anywhere.

bits 64
default rel

extern stub_main
extern x86_regs

; Where this part sits in the protected-mode payload.
%define PART_B_OFFSET 0x8000

section .entry
global _start64
_start64:
    mov [x86_regs + 0 * 8], rax
    mov [x86_regs + 1 * 8], rbx
    mov [x86_regs + 2 * 8], rcx
    mov [x86_regs + 3 * 8], rdx
    mov [x86_regs + 4 * 8], rsi
    mov [x86_regs + 5 * 8], rdi
    mov [x86_regs + 6 * 8], rbp
    mov [x86_regs + 7 * 8], rsp
    lea rsp, [stack_top]
    pushfq
    pop rax
    mov [x86_regs + 8 * 8], rax
    xor eax, eax
    mov ax, cs
    mov [x86_regs + 9 * 8], rax
    mov ax, ds
    mov [x86_regs + 10 * 8], rax
    mov ax, es
    mov [x86_regs + 11 * 8], rax
    mov ax, fs
    mov [x86_regs + 12 * 8], rax
    mov ax, gs
    mov [x86_regs + 13 * 8], rax
    mov ax, ss
    mov [x86_regs + 14 * 8], rax
    mov rax, cr0
    mov [x86_regs + 15 * 8], rax
    mov rax, cr4
    mov [x86_regs + 16 * 8], rax
    mov ecx, 0xc0000080
    rdmsr
    shl rdx, 32
    or rax, rdx
    mov [x86_regs + 17 * 8], rax
    sub rsp, 16
    sgdt [rsp]
    mov rax, [rsp + 2]
    mov [x86_regs + 18 * 8], rax
    movzx eax, word [rsp]
    mov [x86_regs + 19 * 8], rax
    add rsp, 16
    lea rax, [_start64]
    sub rax, PART_B_OFFSET
    mov [x86_regs + 20 * 8], rax
    call stub_main
.halt:
    cli
    hlt
    jmp .halt

section .bss
align 16
    resb 16384
global stack_top
stack_top:
