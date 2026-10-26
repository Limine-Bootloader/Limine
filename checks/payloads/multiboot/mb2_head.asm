; The Multiboot 2 header and entry. The header's tags come from defines:
; INFO_FLAGS (the information request's flags), NO_FB_REQUEST (no request for
; the framebuffer tag, which declares framebuffer support), ADDRESS (with
; LOAD_ADDR and
; LOAD_END), FB (WIDTH, HEIGHT, DEPTH), CONSOLE, MODULE_ALIGN, EFI_BS (its
; flags) and RELOC (RELOC_MIN, RELOC_MAX, RELOC_ALIGN, RELOC_PREF). The code
; is position independent so the relocatable header tag can move it.

bits 32

extern mb2_main
extern _GLOBAL_OFFSET_TABLE_
extern __load_start, __load_end, __bss_end
global _start, efi_entry
global mb_regs, mb_gdtr, mb_efi_entry

%ifndef INFO_FLAGS
%define INFO_FLAGS 0
%endif

section .multiboot
align 8
mb_header:
    dd 0xe85250d6
    dd 0
    dd mb_header_end - mb_header
    dd -(0xe85250d6 + (mb_header_end - mb_header))

align 8, db 0
info_request:
    dw 1, INFO_FLAGS
    dd info_request_end - info_request
    dd 1, 2, 3, 4, 6
%ifndef NO_FB_REQUEST
    dd 8
%endif
    dd 9, 11, 12, 13, 14, 15, 17, 19, 20, 21
info_request_end:

%ifdef ADDRESS
align 8, db 0
    dw 2, 0
    dd 24
    dd mb_header
    dd LOAD_ADDR
    dd LOAD_END
    dd __bss_end
align 8, db 0
    dw 3, 0
    dd 12
    dd _start
%endif

%ifdef FB
align 8, db 0
    dw 5, 0
    dd 20
    dd WIDTH, HEIGHT, DEPTH
%endif

%ifdef CONSOLE
align 8, db 0
    dw 4, 0
    dd 12
    dd CONSOLE
%endif

%ifdef MODULE_ALIGN
align 8, db 0
    dw 6, 0
    dd 8
%endif

%ifdef EFI_BS
align 8, db 0
    dw 7, EFI_BS
    dd 8
align 8, db 0
    dw 8, 1
    dd 12
    dd efi_entry
align 8, db 0
    dw 9, 1
    dd 12
    dd efi_entry
%endif

%ifdef RELOC
align 8, db 0
    dw 10, 0
    dd 24
    dd RELOC_MIN, RELOC_MAX, RELOC_ALIGN, RELOC_PREF
%endif

align 8, db 0
    dw 0, 0
    dd 8
mb_header_end:

section .text
efi_entry:
    mov ecx, 1
    jmp entry
_start:
    xor ecx, ecx
entry:
    ; No stack is promised. Finding out where we run takes a call, so borrow
    ; the information structure's reserved field for it, putting it back after.
    mov edi, [ebx + 4]
    lea esp, [ebx + 8]
    pushfd
    pop edx
    call .here
.here:
    pop esi
    mov [ebx + 4], edi
    add esi, _GLOBAL_OFFSET_TABLE_ + $$ - .here wrt ..gotpc
    lea esp, [esi + stack_top wrt ..gotoff]
    mov [esi + mb_efi_entry wrt ..gotoff], ecx
    lea edi, [esi + mb_regs wrt ..gotoff]
    mov [edi + 0 * 4], eax
    mov [edi + 1 * 4], ebx
    mov [edi + 3 * 4], edx
    xor eax, eax
    mov ax, cs
    mov [edi + 4 * 4], eax
    mov ax, ds
    mov [edi + 5 * 4], eax
    mov ax, es
    mov [edi + 6 * 4], eax
    mov ax, fs
    mov [edi + 7 * 4], eax
    mov ax, gs
    mov [edi + 8 * 4], eax
    mov ax, ss
    mov [edi + 9 * 4], eax
    mov eax, cr0
    mov [edi + 10 * 4], eax
    lea eax, [esi + mb_gdtr wrt ..gotoff]
    sgdt [eax]
    call mb2_main
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
mb_efi_entry:
    dd 0

section .bss
align 16
    resb 16384
stack_top:
