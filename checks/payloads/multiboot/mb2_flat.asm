; A flat Multiboot 2 image whose header, with address and entry address tags
; and 64 bytes long, sits at HEADER_OFFSET in the file: what Limine accepts
; around the end of the first 32768 bytes. It only says that it ran.

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
    db 10, 'B MB2-FLAT', 10, 'E DONE', 10, 0

    times HEADER_OFFSET - ($ - $$) db 0
header:
    dd 0xe85250d6
    dd 0
    dd header_end - header
    dd -(0xe85250d6 + (header_end - header))
    dw 2, 0
    dd 24
    dd header
    dd start
    dd 0
    dd 0
    dw 3, 0
    dd 12
    dd start
    dd 0
    dw 0, 0
    dd 8
header_end:
    times 64 db 0
