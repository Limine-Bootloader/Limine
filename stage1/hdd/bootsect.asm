org 0x7c00
bits 16

start:
    jmp .skip_bpb ; Workaround for some BIOSes that require this stub
    nop

    ; Some BIOSes will do a funny and decide to overwrite bytes of code in
    ; the section where a FAT BPB would be, potentially overwriting
    ; bootsector code.
    ; Avoid that by filling the BPB area with dummy values.
    ; Some of the values have to be set to certain values in order
    ; to boot on even quirkier machines.
    ; Source: https://github.com/freebsd/freebsd-src/blob/82a21151cf1d7a3e9e95b9edbbf74ac10f386d6a/stand/i386/boot2/boot1.S
  .bpb:
    times 3-($-$$) db 0
    .bpb_oem_id:            db "LIMINE  "
    .bpb_sector_size:       dw 512
    .bpb_sects_per_cluster: db 0
    .bpb_reserved_sects:    dw 0
    .bpb_fat_count:         db 0
    .bpb_root_dir_entries:  dw 0
    .bpb_sector_count:      dw 0
    .bpb_media_type:        db 0
    .bpb_sects_per_fat:     dw 0
    .bpb_sects_per_track:   dw 18
    .bpb_heads_count:       dw 2
    .bpb_hidden_sects:      dd 0
    .bpb_sector_count_big:  dd 0
    .bpb_drive_num:         db 0
    .bpb_reserved:          db 0
    .bpb_signature:         db 0
    .bpb_volume_id:         dd 0
    .bpb_volume_label:      db "LIMINE     "
    .bpb_filesystem_type:   times 8 db 0

  .skip_bpb:
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

    ; Limine isn't made for floppy disks, these are dead anyways.
    ; So if the value the BIOS passed is <0x80, just assume it has passed
    ; an incorrect value.
    cmp dl, 0x80
    jb err.0
    ; Values above 0x8f are dubious so we assume we weren't booted properly
    ; for those either
    cmp dl, 0x8f
    ja err.1

  .continue:
    ; Make sure int 13h extensions are supported
    mov ah, 0x41
    mov bx, 0x55aa
    int 0x13
    jc err.2
    cmp bx, 0xaa55
    jne err.3

    push 0x7000
    pop es
    mov di, stage2_loc
    mov eax, dword [di]
    mov ebp, dword [di+4]
    xor bx, bx
    mov ecx, 32256 ; 32KiB minus boot sector size
    call read_sectors
    jc err.4

    lgdt [gdt]

    cli

    push dword 0
    mov ebp, 0x10

    mov eax, cr0
    bts ax, 0
    mov cr0, eax

    jmp 0x08:vector

times 0xda-($-$$) db 0
times 6 db 0

; Includes

%include 'disk.asm'
%include '../gdt.asm'

err:
  .4:
    inc si
  .3:
    inc si
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

bits 32
vector:
    mov ds, ebp
    mov es, ebp
    mov fs, ebp
    mov gs, ebp
    mov ss, ebp

    and edx, 0xff

    push edx

    push stage2.size
    push (stage2 - decompressor) + 0x70000

    call 0x70000

times 0x1a4-($-$$) db 0
stage2_loc: dq 0

times 0x1b8-($-$$) db 0
times 510-($-$$) db 0
dw 0xaa55

; ********************* Stage 2 *********************

decompressor:
%strcat DECOMPRESSOR_PATH BUILDDIR, '/decompressor-build/decompressor.bin'
incbin DECOMPRESSOR_PATH

align 16
stage2:
%strcat STAGE2_PATH BUILDDIR, '/common-bios/stage2.bin.limlz'
incbin STAGE2_PATH
.size: equ $ - stage2
