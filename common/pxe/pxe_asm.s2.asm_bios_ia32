section .realmode

global pxe_call
global set_pxe_fp

set_pxe_fp:
    mov eax, [esp + 4]
    mov [pxe_call.pxe_fp], eax
    ret

pxe_call:
    ; Save GDT in case BIOS overwrites it
    sgdt [.gdt]

    ; Save IDT
    sidt [.idt]

    ; Load BIOS IVT
    lidt [.rm_idt]

    ; Save non-scratch GPRs
    push ebx
    push esi
    push edi
    push ebp

    lea ebp, [esp + 20]

    ; Jump to real mode
    jmp 0x08:.bits16
  .bits16:
    bits 16
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov eax, cr0
    and al, 0xfe
    mov cr0, eax
    jmp 0x00:.cszero
  .cszero:
    xor ax, ax
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    sti

    push word [bp + 4]
    push word [bp + 8]
    push word [bp + 0]
    call far [.pxe_fp]
    add sp, 6

    cli

    ; Restore GDT
    o32 lgdt [cs:.gdt]

    ; Restore IDT
    o32 lidt [cs:.idt]

    ; Jump back to pmode
    mov ebx, cr0
    or bl, 1
    mov cr0, ebx
    jmp 0x18:.bits32
  .bits32:
    bits 32
    mov bx, 0x20
    mov ds, bx
    mov es, bx
    mov fs, bx
    mov gs, bx
    mov ss, bx

    and eax, 0xffff

    ; Restore non-scratch GPRs
    pop ebp
    pop edi
    pop esi
    pop ebx

    ; Exit
    ret

align 16
  .pxe_fp:   dd 0
  .gdt:      dq 0
  .idt:      dq 0
  .rm_idt:   dw 0x3ff
             dd 0
