org 0x7c00
bits 16

start:
    cli
    cld
    jmp 0x0000:.initialise_cs
  .initialise_cs:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00

    lgdt [gdt]

    mov eax, cr0
    bts ax, 0
    mov cr0, eax

    jmp 0x08:.mode32
    bits 32
  .mode32:
    mov eax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    push 0x1
    and edx, 0xff
    push edx

    push stage2.size
    push (stage2 - decompressor) + 0x70000

    mov esi, decompressor
    mov edi, 0x70000
    mov ecx, stage2.fullsize
    rep movsb

    call 0x70000

; Includes

%include '../gdt.asm'

; ********************* Stage 2 *********************

decompressor:
%strcat DECOMPRESSOR_PATH BUILDDIR, '/decompressor-build/decompressor.bin'
incbin DECOMPRESSOR_PATH

align 16
stage2:
%strcat STAGE2_PATH BUILDDIR, '/common-bios/stage2.bin.limlz'
incbin STAGE2_PATH
.size: equ $ - stage2
.fullsize: equ $ - decompressor
