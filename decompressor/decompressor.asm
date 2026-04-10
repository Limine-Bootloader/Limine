; limlz: Copyright (C) 2026 Kamila Szewczyk <k@iczelia.net>
; limine: Copyright (C) 2019-2026 Mintsuki and contributors.
; 
; Redistribution and use in source and binary forms, with or without
; modification, are permitted provided that the following conditions are met:
; 
; 1. Redistributions of source code must retain the above copyright notice, this
;    list of conditions and the following disclaimer.
; 
; 2. Redistributions in binary form must reproduce the above copyright notice,
;    this list of conditions and the following disclaimer in the documentation
;    and/or other materials provided with the distribution.
; 
; THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
; ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
; WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
; DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
; FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
; DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
; SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
; CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
; OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
; OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

bits 32

section .entry progbits alloc exec nowrite align=16

global _start
_start:
    cld
    ; On stack (cdecl): [esp+4]=compressed_stage2, [esp+8]=stage2_size,
    ;                   [esp+12]=boot_drive (byte), [esp+16]=pxe
    mov    ebx, dword [esp+0x4]      ; compressed_stage2
    mov    ebp, dword [ebx]          ; expected_crc = *(uint32_t *)compressed_stage2
    lea    edx, [ebx+0x4]            ; ip = compressed_stage2 + 4
    add    ebx, dword [esp+0x8]      ; ipe = compressed_stage2 + stage2_size
    mov    edi, 0xf000               ; op = dest
    ; LZ decompression loop
.Ltoken:
    movzx  ecx, byte [edx]
    lea    esi, [edx+0x1]
    mov    eax, ecx                  ; save token
    shr    ecx, 0x3
    and    ecx, 0xf                  ; literal length = (token >> 3) & 15
    cmp    ecx, 0xf
    jne    .Llitcopy
    movzx  ecx, byte [edx+0x1]
    lea    esi, [edx+0x2]
    add    ecx, 0xf                  ; length += extra byte + 15
.Llitcopy:
    rep    movsb                     ; copy literals
    cmp    esi, ebx
    jae    .Lcrc                     ; if ip >= ipe, done
    test   al, al
    jns    .Loffset1                 ; bit 7 clear => 1-byte offset
    lea    edx, [esi+0x2]
    movzx  esi, word [esi]           ; 2-byte offset
    jmp    .Lmatchlen
.Loffset1:
    lea    edx, [esi+0x1]
    movzx  esi, byte [esi]           ; 1-byte offset
.Lmatchlen:
    and    al, 0x7
    cmp    al, 0x7
    je     .Lmatchextra
    movzx  eax, al
    jmp    .Ldomatch
.Lmatchextra:
    movzx  eax, byte [edx]
    inc    edx
    add    eax, 0x7                  ; matchlen += extra byte + 7
.Ldomatch:
    mov    ecx, edi
    sub    ecx, esi                  ; match = op - offset
    mov    esi, ecx
    lea    ecx, [eax+0x4]            ; count = matchlen + 4
    rep    movsb                     ; copy match
    jmp    .Ltoken
    ; CRC32 verification
.Lcrc:
    mov    edx, 0xf000               ; ptr = dest
    mov    esi, edx                  ; (also reused for esp later)
    xor    eax, eax
    dec    eax
.Lcrc_byte:
    cmp    edx, edi
    je     .Lcrc_done
    lea    ecx, [edx+0x1]
    movzx  edx, byte [edx]
    xor    eax, edx
    push   0x08
    pop    edx                       ; 8 bits per byte
.Lcrc_bit:
    mov    ebx, eax
    and    eax, 0x1
    shr    ebx, 1
    neg    eax
    and    eax, 0xedb88320
    xor    eax, ebx
    dec    edx
    jne    .Lcrc_bit
    mov    edx, ecx
    jmp    .Lcrc_byte
.Lcrc_done:
    not    eax
    cmp    eax, ebp
    jne    .Lerror
    ; Jump to decompressed stage2
    movzx  eax, byte [esp+0xc]       ; boot_drive
    mov    ecx, dword [esp+0x10]     ; pxe
    mov    esp, esi
    xor    ebp, ebp
    push   ecx
    push   eax
    push   ebp
    push   esi
    ret                              ; jump to 0xf000
    ; Error: display message and cli/hlt
.Lerror:
    mov    edx, errmsg
    mov    eax, 0xb8000
.Lerror_loop:
    movzx  ecx, byte [edx]
    add    eax, 0x2
    inc    edx
    or     ch, 0x4f
    mov    word [eax-0x2], cx
    cmp    eax, 0xB8000 + errmsg.len * 2
    jne    .Lerror_loop
    cli
    hlt

section .rodata progbits alloc noexec nowrite align=1

errmsg: db "limine integrity error"
.len: equ $ - errmsg - 1

section .note.GNU-stack noalloc noexec nowrite progbits
