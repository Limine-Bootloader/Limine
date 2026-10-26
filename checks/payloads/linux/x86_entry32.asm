; The 32-bit boot protocol's entry: protected mode, paging off, %esi the zero
; page, and no stack. Position independent, as a relocatable kernel may be
; loaded anywhere.

bits 32

extern stub_main
extern x86_regs
extern _GLOBAL_OFFSET_TABLE_

; Where this part sits in the protected-mode payload.
%define PART_A_OFFSET 0x400

section .entry
global _start32
_start32:
    ; Finding out where we run takes a call, so borrow the zero page's 4-byte
    ; scratch field (Linux Documentation/arch/x86/zero-page.rst) as its stack.
    lea esp, [esi + 0x1e4 + 4]
    pushfd
    pop ecx
    call .here
.here:
    pop edx
    lea eax, [edx - (.here - _start32) - PART_A_OFFSET]
    add edx, _GLOBAL_OFFSET_TABLE_ + $$ - .here wrt ..gotpc
    lea esp, [edx + stack_top wrt ..gotoff]
    push eax
    lea eax, [edx + x86_regs wrt ..gotoff]
    mov [eax + 1 * 8], ebx
    mov [eax + 4 * 8], esi
    mov [eax + 5 * 8], edi
    mov [eax + 6 * 8], ebp
    mov [eax + 8 * 8], ecx
    pop ecx
    mov [eax + 20 * 8], ecx
    xor ecx, ecx
    mov cx, cs
    mov [eax + 9 * 8], ecx
    mov cx, ds
    mov [eax + 10 * 8], ecx
    mov cx, es
    mov [eax + 11 * 8], ecx
    mov cx, fs
    mov [eax + 12 * 8], ecx
    mov cx, gs
    mov [eax + 13 * 8], ecx
    mov cx, ss
    mov [eax + 14 * 8], ecx
    mov ecx, cr0
    mov [eax + 15 * 8], ecx
    mov ecx, cr4
    mov [eax + 16 * 8], ecx
    sub esp, 8
    sgdt [esp]
    mov ecx, [esp + 2]
    mov [eax + 18 * 8], ecx
    movzx ecx, word [esp]
    mov [eax + 19 * 8], ecx
    add esp, 8
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
