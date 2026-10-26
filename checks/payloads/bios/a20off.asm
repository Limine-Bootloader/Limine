; A boot record that turns the A20 gate off through port 0x92 and boots the
; second disk's MBR in its place, saying on port 0xe9 whether the gate was
; still off when it did. With BREAK_INT15 it also makes INT 15h AX=2401h, the
; BIOS's way of turning the gate on, fail.

bits 16
org 0x600

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    ; Out of the way of the MBR it loads.
    mov si, 0x7c00
    mov di, 0x600
    mov cx, 256
    cld
    rep movsw
    jmp 0:relocated

relocated:
    sti
%ifdef BREAK_INT15
    mov eax, [0x15 * 4]
    mov [old_int15], eax
    cli
    mov word [0x15 * 4], int15
    mov word [0x15 * 4 + 2], 0
    sti
%endif

    in al, 0x92
    ; Bit 0 resets the machine.
    and al, ~0x03
    out 0x92, al

    mov si, dap
    mov dl, 0x81
    mov ah, 0x42
    int 0x13
    jc fail

    ; With the gate off, 0xffff:0x510 is 0:0x500.
    push ds
    mov ax, 0xffff
    mov ds, ax
    mov byte [es:0x500], 0x5a
    cmp byte [0x510], 0x5a
    jne .on
    mov byte [es:0x500], 0xa5
    cmp byte [0x510], 0xa5
    jne .on
    pop ds
    mov si, s_off
    jmp .say
.on:
    pop ds
    mov si, s_on
.say:
    call puts
    mov dl, 0x81
    jmp 0:0x7c00

fail:
    mov si, s_fail
    call puts
.halt:
    cli
    hlt
    jmp .halt

puts:
    lodsb
    test al, al
    jz .done
    out 0xe9, al
    jmp puts
.done:
    ret

%ifdef BREAK_INT15
int15:
    cmp ax, 0x2401
    je .refuse
    jmp far [cs:old_int15]
.refuse:
    mov ah, 0x86
    push bp
    mov bp, sp
    ; CF in the FLAGS image the IRET pops.
    or byte [bp + 6], 1
    pop bp
    iret

old_int15: dd 0
%endif

dap:
    db 0x10, 0
    dw 1
    dw 0x7c00, 0
    dq 0

s_off: db "A20 off before Limine", 10, 0
s_on: db "A20 on before Limine", 10, 0
s_fail: db "A20 boot record: disk read failed", 10, 0

times 440-($-$$) db 0
times 510-($-$$) db 0
dw 0xaa55
