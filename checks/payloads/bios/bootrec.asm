; A boot record that reports how it was entered on port 0xe9, then halts.
; Fits the 440 bytes of MBR code, so the same sector serves as an MBR, a
; partition's boot record, or the first sector of a freebsd-boot partition.
; The harness patches the 8 name bytes to tell copies apart.

bits 16
org 0x7c00

start:
    ; The entry state goes on the stack, to be popped in the order of names.
    pushf
    push ss
    push es
    push ds
    push si
    push dx
    mov bp, sp
    lea ax, [bp + 12]
    push ax
    call .here
.here:
    pop ax
    sub ax, .here - start
    push ax
    push cs

    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    ; DS:SI as entered, for the partition entry dump.
    mov bx, [bp + 2]
    mov di, [bp + 4]

    mov si, s_begin
    call puts
    mov si, s_name
    call puts_fact
    mov si, name
    mov cx, 8
.name:
    lodsb
    call putc
    loop .name
    call newline

    mov si, names
    mov cx, 9
.reg:
    call puts_fact
    pop ax
    call hex16
    call newline
    loop .reg

    ; The partition entry DS:SI pointed at.
    call puts_fact
    mov es, di
    mov cx, 16
    call dump

    xor ax, ax
    mov es, ax
    call puts_fact
    mov bx, 0x7be
    mov cx, 64
    call dump

    call puts_fact
    mov bx, 0x7c00 + 0x2000
    mov cx, 16
    call dump

    call puts
.halt:
    hlt
    jmp .halt

; es:bx, cx bytes, then a newline.
dump:
    mov al, [es:bx]
    call hex8
    inc bx
    loop dump
    jmp newline

hex16:
    xchg al, ah
    call hex8
    xchg al, ah
hex8:
    push ax
    shr al, 4
    call nibble
    pop ax
    push ax
    and al, 15
    call nibble
    pop ax
    ret

nibble:
    add al, '0'
    cmp al, '9'
    jbe putc
    add al, 'a' - '9' - 1
putc:
    out 0xe9, al
    ret

newline:
    mov al, 10
    jmp putc

; "K " and then the string at si, which is left past its NUL. Clobbers al.
puts_fact:
    mov al, 'K'
    call putc
    mov al, ' '
    call putc
puts:
    lodsb
    test al, al
    jz .ret
    call putc
    jmp puts
.ret:
    ret

s_begin: db 10, 'B BOOTREC', 10, 0
s_name: db 'name=', 0
names:
    db 'cs=', 0, 'ip=', 0, 'sp=', 0, 'dx=', 0, 'si=', 0, 'ds=', 0, 'es=', 0, 'ss=', 0, 'flags=', 0
    db 'entry=', 0, 'table=', 0, 'tail=', 0
    db 'E DONE', 10, 0

name: db 'BOOTREC_'

%if ($ - start) > 440
%error "boot record code overlaps the disk signature"
%endif

    times 446 - ($ - start) db 0
    times 510 - ($ - start) db 0
    dw 0xaa55
