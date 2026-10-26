; A flat Multiboot 1 image whose header, with its address fields, sits at
; HEADER_OFFSET in the file: what Limine accepts around the end of the first
; 8192 bytes. It only says that it ran.

bits 32
org 0x300000

start:
    mov edx, 0xe9
    mov esi, msg
.next:
    lodsb
    test al, al
    jz .halt
    out dx, al
    jmp .next
.halt:
    cli
    hlt
    jmp .halt

msg:
    db 10, 'B MB1-FLAT', 10, 'E DONE', 10, 0

    times HEADER_OFFSET - ($ - $$) db 0
header:
    dd 0x1badb002
    dd 0x10000
    dd -(0x1badb002 + 0x10000)
    dd header
    dd start
    dd 0
    dd 0
    dd start
    times 64 db 0
