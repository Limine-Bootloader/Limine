#include "state.h"

.intel_syntax noprefix

// Takes the snapshot into the slots rax points to. On entry the stack holds
// the original rax on top of the original rflags; both are popped, leaving rsp
// as it was at entry.
.macro CAPTURE
    mov [rax + ST_RBX * 8], rbx
    mov [rax + ST_RCX * 8], rcx
    mov [rax + ST_RDX * 8], rdx
    mov [rax + ST_RSI * 8], rsi
    mov [rax + ST_RDI * 8], rdi
    mov [rax + ST_RBP * 8], rbp
    mov [rax + (ST_R8 + 0) * 8], r8
    mov [rax + (ST_R8 + 1) * 8], r9
    mov [rax + (ST_R8 + 2) * 8], r10
    mov [rax + (ST_R8 + 3) * 8], r11
    mov [rax + (ST_R8 + 4) * 8], r12
    mov [rax + (ST_R8 + 5) * 8], r13
    mov [rax + (ST_R8 + 6) * 8], r14
    mov [rax + (ST_R8 + 7) * 8], r15
    pop rbx
    mov [rax + ST_RAX * 8], rbx
    pop rbx
    mov [rax + ST_RFLAGS * 8], rbx
    mov [rax + ST_RSP * 8], rsp
    mov rbx, [rsp]
    mov [rax + ST_RET * 8], rbx
    xor ebx, ebx
    mov bx, cs
    mov [rax + ST_CS * 8], rbx
    mov bx, ds
    mov [rax + ST_DS * 8], rbx
    mov bx, es
    mov [rax + ST_ES * 8], rbx
    mov bx, ss
    mov [rax + ST_SS * 8], rbx
    mov bx, fs
    mov [rax + ST_FS * 8], rbx
    mov bx, gs
    mov [rax + ST_GS * 8], rbx
    mov rbx, cr0
    mov [rax + ST_CR0 * 8], rbx
    mov rbx, cr2
    mov [rax + ST_CR2 * 8], rbx
    mov rbx, cr3
    mov [rax + ST_CR3 * 8], rbx
    mov rbx, cr4
    mov [rax + ST_CR4 * 8], rbx
    mov rbx, cr8
    mov [rax + ST_CR8 * 8], rbx
    sgdt [rax + ST_GDTR * 8]
    sidt [rax + ST_IDTR * 8]
    xor ebx, ebx
    str bx
    mov [rax + ST_TR * 8], rbx
    sldt bx
    mov [rax + ST_LDTR * 8], rbx
    mov rsi, rax
    mov ecx, 0xc0000080
    rdmsr
    shl rdx, 32
    or rax, rdx
    mov [rsi + ST_EFER * 8], rax
    mov ecx, 0xc0000100
    rdmsr
    shl rdx, 32
    or rax, rdx
    mov [rsi + ST_FS_BASE * 8], rax
    mov ecx, 0xc0000101
    rdmsr
    shl rdx, 32
    or rax, rdx
    mov [rsi + ST_GS_BASE * 8], rax
    mov ecx, 0xc0000102
    rdmsr
    shl rdx, 32
    or rax, rdx
    mov [rsi + ST_KGS_BASE * 8], rax
    mov ecx, 0x277
    rdmsr
    shl rdx, 32
    or rax, rdx
    mov [rsi + ST_PAT * 8], rax
    mov ecx, 0x1b
    rdmsr
    shl rdx, 32
    or rax, rdx
    mov [rsi + ST_APIC_BASE * 8], rax
.endm

.section .text

.globl _start
_start:
    pushfq
    push rax
    mov qword ptr [rip + entry_id], ENTRY_ELF
    lea rax, [rip + bsp_state]
    CAPTURE
    and rsp, -16
    call kmain
1:
    cli
    hlt
    jmp 1b

.globl entry_req
entry_req:
    pushfq
    push rax
    mov qword ptr [rip + entry_id], ENTRY_REQUEST
    lea rax, [rip + bsp_state]
    CAPTURE
    and rsp, -16
    call kmain
1:
    cli
    hlt
    jmp 1b

// Entered through goto_address with the processor's limine_mp_info in rdi,
// whose extra_argument the BSP set to the processor's slot.
.globl ap_entry
ap_entry:
    pushfq
    push rax
    mov rax, [rdi + 24]
    imul rax, rax, ST_COUNT * 8
    push rbx
    lea rbx, [rip + ap_states]
    add rax, rbx
    pop rbx
    CAPTURE
    mov rdi, [rsi + ST_RDI * 8]
    and rsp, -16
    call ap_main
1:
    cli
    hlt
    jmp 1b
