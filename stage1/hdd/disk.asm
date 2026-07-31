; *****************************
;     Reads bytes from disk
; *****************************

; IN:
; EAX = Start address to load low 32
; EBP = Start address to load high 32
; DL = Drive number
; ES = Buffer segment
; BX = Buffer offset
; ECX = Byte count

; OUT:
; Carry if error

read_sectors:
    pusha

    mov si, .da_struct

    mov dword [si],    0x00010010
    mov word  [si+4],  bx
    mov word  [si+6],  es

    push dx
    push si

    push eax
    push ebp

    ; Get bytes per sector
    mov ah, 0x48
    mov si, .drive_params
    mov word [si], 30       ; buf_size
    mov word [si+2], 0      ; PhoenixBIOS 4.0 R6.0 fails AH=48h unless it is clear
    int 0x13
    jc .fail
    movzx ebp, word [si+24] ; bytes_per_sect

    ; ECX byte count to CX sector count
    xchg ax, cx
    shr ecx, 16
    mov dx, cx
    xor cx, cx
    div bp
    test dx, dx
    setnz cl
    add cx, ax

    pop edx
    pop eax

    pop si

    ; EDX:EAX byte address to 64-bit LBA sector
    push eax
    xchg eax, edx
    xor edx, edx
    div ebp
    xchg ebx, eax
    pop eax
    div ebp
    mov dword [si+8],  eax
    mov dword [si+12], ebx

    pop dx

  .loop:
    mov ah, 0x42

    clc
    int 0x13
    jc .done

    add word  [si+4], bp
    add dword [si+8], 1
    adc dword [si+12], 0

    loop .loop

    jmp short .done

  .fail:
    add sp, 12
    stc
  .done:
    popa
    ret

  .da_struct:    equ 0x8000
  .drive_params: equ 0x8010
