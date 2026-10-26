; For checking where the PICs deliver IRQs: a GDT of our own, as Multiboot
; does not promise a usable one, and an IDT whose handlers note the vector
; that arrived. Nothing here pushes an error code, so only IRQs may come in.

bits 32

global mb_idt_install, mb_irq_vector, mb_irq_count

section .text

%assign i 0
%rep 256
stub_ %+ i:
    push dword i
    jmp near irq_common
%assign i i + 1
%endrep

irq_common:
    push eax
    mov eax, [esp + 4]
    mov [mb_irq_vector], eax
    inc dword [mb_irq_count]
    cmp eax, 0x70
    jb .master
    cmp eax, 0x77
    ja .master
    ; The RTC raises no further interrupt until register C is read.
    mov al, 0x0c
    out 0x70, al
    in al, 0x71
    mov al, 0x20
    out 0xa0, al
.master:
    mov al, 0x20
    out 0x20, al
    pop eax
    add esp, 4
    iretd

mb_idt_install:
    lgdt [gdtr]
    jmp 0x08:.reload
.reload:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    xor ecx, ecx
.entry:
    mov eax, [stub_table + ecx * 4]
    mov [idt + ecx * 8], ax
    mov word [idt + ecx * 8 + 2], 0x08
    mov word [idt + ecx * 8 + 4], 0x8e00
    shr eax, 16
    mov [idt + ecx * 8 + 6], ax
    inc ecx
    cmp ecx, 256
    jb .entry
    lidt [idtr]
    ret

section .data
align 8
gdt:
    dq 0
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
gdtr:
    dw 3 * 8 - 1
    dd gdt
align 4
idtr:
    dw 256 * 8 - 1
    dd idt
mb_irq_vector:
    dd 0xffffffff
mb_irq_count:
    dd 0
stub_table:
%assign i 0
%rep 256
    dd stub_ %+ i
%assign i i + 1
%endrep

section .bss
align 8
idt:
    resb 256 * 8
