org 0x7c00
bits 16

jmp skip_bpb
nop

; El Torito Boot Information Table
; ↓ Set by mkisofs
times 8-($-$$) db 0
boot_info:
    bi_PVD      dd 0
    bi_boot_LBA dd 0
    bi_boot_len dd 0
    bi_checksum dd 0
    bi_reserved times 40 db 0

times 90-($-$$) db 0

skip_bpb:
    cli
    cld
    jmp 0x0000:.initialise_cs
  .initialise_cs:
    xor si, si
    mov ds, si
    mov es, si
    mov ss, si
    mov sp, 0x7c00
    sti

    ; int 13h?
    mov ah, 0x41
    mov bx, 0x55aa
    int 0x13
    jc err.0
    cmp bx, 0xaa55
    jne err.1

    ; --- Load the decompressor ---
    mov eax, dword [bi_boot_LBA]
    add eax, 1
    mov ecx, stage2.fullsize / 2048
    ; DECOMPRESSOR_LOCATION = 0x70000 = 0x7000:0x0000
    push 0x7000
    pop es
    xor bx, bx
    call read_2k_sectors
    jc err.2

    ; Enable GDT
    lgdt [gdt]
    cli
    mov eax, cr0
    or al, 1
    mov cr0, eax

    jmp 0x08:pmode

err:
  .2:
    inc si
  .1:
    inc si
  .0:
    add si, '0' | (0x4f << 8)

    push 0xb800
    pop es
    mov word [es:0], si

    sti
    .h: hlt
    jmp .h

%include 'read_2k_sectors.asm'
%include '../gdt.asm'

bits 32
pmode:
    mov eax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Time to handle control over to the decompressor
    push 2
    and edx, 0xff
    push edx  ; Boot drive
    push stage2.size
    push (stage2 - decompressor) + 0x70000
    call 0x70000

; Align stage2 to 2K ON DISK
times 2048-($-$$) db 0
decompressor:
%strcat DECOMPRESSOR_PATH BUILDDIR, '/decompressor-build/decompressor.bin'
incbin DECOMPRESSOR_PATH

align 16
stage2:
%strcat STAGE2_PATH BUILDDIR, '/common-bios/stage2.bin.limlz'
incbin STAGE2_PATH
.size: equ $ - stage2

times ((($-$$)+2047) & ~2047)-($-$$) db 0
.fullsize: equ $ - decompressor
