; limlz: Copyright (C) 2026 Kamila Szewczyk <k@iczelia.net>
; limine: Copyright (C) 2019-2026 Mintsuki and contributors.
;
; SPDX-License-Identifier: BSD-2-Clause AND CC-BY-3.0
;
; The algorithm is based on LZMA, augmented with a x86 filter and a
; Storer-Szymanski backwards optimal parse.
;
; The decoder is a modified version of the Micro LZMA decoder for x86 (static)
; by Ilya Kurdyukov, https://github.com/ilyakurdyukov/micro-lzmadec. The
; modifications are under the terms below; its original notice follows them.
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
;
; Copyright (c) 2022, Ilya Kurdyukov
; All rights reserved.
;
; Micro LZMA decoder for x86 (static)
;
; This software is distributed under the terms of the
; Creative Commons Attribution 3.0 License (CC-BY 3.0)
; http://creativecommons.org/licenses/by/3.0/
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
; OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
; THE SOFTWARE.

%define LOAD_ADDRESS 0x70000

org LOAD_ADDRESS
bits 32

%ifndef OUTPUT
%define OUTPUT 0xf000
%endif
%ifndef CAPACITY
%define CAPACITY (LOAD_ADDRESS - OUTPUT)
%endif

; Original CRC32, decoded size, then x86 BCJ and raw LZMA1 (lc=2, lp=0, pb=0).
%define LIMLZ_HEADER_SIZE 8
%define RANGE_INIT_SIZE 5
%define STREAM_MIN_SIZE (LIMLZ_HEADER_SIZE + RANGE_INIT_SIZE)

; Model offsets match tools/limlzpack.c; position tables reserve 16 states.
%define LIMLZ_IS_REP 192
%define LIMLZ_REP_G0 204
%define LIMLZ_REP_G1 216
%define LIMLZ_REP_G2 228
%define LIMLZ_REP_LONG 240
%define LIMLZ_SLOT 432
%define LIMLZ_SPECIAL 688
%define LIMLZ_ALIGN 802
%define LIMLZ_LENGTH 818
%define LIMLZ_REP_LENGTH 1332
%define LIMLZ_LITERAL 1846
%define LITERAL_CONTEXT_BITS 2
%define LITERAL_CONTEXT_SIZE 768
%define PROBS (LIMLZ_LITERAL + (LITERAL_CONTEXT_SIZE << LITERAL_CONTEXT_BITS))
%define POS_STATE_BITS 4
%define PROB_BITS 11
%define PROB_TOTAL (1 << PROB_BITS)
%define PROB_MOVE_BITS 5
%define LITERAL_MATCH_BIT 256
%define SLOT_BITS 6
%define SLOT_MODEL_START 4
%define SLOT_MODEL_END 14
%define ALIGN_BITS 4
%define LENGTH_STATES 4
%define MATCH_MIN 2
%define LENGTH_LOW_BITS 3
%define LENGTH_MID_BITS 3
%define LENGTH_HIGH_BITS 8
%define LENGTH_LOW_SIZE (1 << LENGTH_LOW_BITS)
%define LENGTH_MID_SIZE (1 << LENGTH_MID_BITS)
%define LENGTH_HIGH_SIZE (1 << LENGTH_HIGH_BITS)
%define LENGTH_LOW 2
%define LENGTH_MID (LENGTH_LOW + (LENGTH_LOW_SIZE << POS_STATE_BITS))
%define LENGTH_HIGH (LENGTH_MID + (LENGTH_MID_SIZE << POS_STATE_BITS))

%define BCJ_CALL 0xe8
%define BCJ_OPERAND_SIZE 4
%define BCJ_HISTORY_MASK 0x77
%define BCJ_MAX_HISTORY 8
%define BCJ_INVALID_HISTORY 6
%define BCJ_SIGN_HISTORY 16
%define BCJ_SIGN_BITS 25

%define LOCAL_SIZE 44
%define REP_OFFSET(n) (-24 - 4 * (n))
%define SOURCE dword [ebp-4]
%define SOURCE_END dword [ebp-8]
%define RANGE dword [ebp-12]
%define CODE dword [ebp-16]
%define STATE dword [ebp-20]
%define REP0 dword [ebp+REP_OFFSET(0)]
%define REP1 dword [ebp+REP_OFFSET(1)]
%define REP2 dword [ebp+REP_OFFSET(2)]
%define REP3 dword [ebp+REP_OFFSET(3)]
%define RANGE_HIGH byte [ebp-9]
%define CODE_LOW byte [ebp-16]
%define OUTPUT_END dword [ebp-40]
%define CHECKSUM dword [ebp-44]

global _start
_start:
    ; cdecl: stream, stream_size, boot_drive, boot_type.
    cld
    mov esi, [esp+4]
    mov ecx, [esp+8]
    cmp ecx, STREAM_MIN_SIZE
    jb error
    cmp byte [esi+LIMLZ_HEADER_SIZE], 0
    jne error
    mov eax, [esi+4]
    cmp eax, CAPACITY
    ja error
    sub esp, PROBS * 2
    mov ebp, esp
    sub esp, LOCAL_SIZE
    add eax, OUTPUT
    mov OUTPUT_END, eax
    mov eax, [esi]
    mov CHECKSUM, eax
    add ecx, esi
    mov SOURCE_END, ecx
    mov eax, [esi+LIMLZ_HEADER_SIZE+1]
    bswap eax
    mov CODE, eax
    add esi, STREAM_MIN_SIZE
    mov SOURCE, esi
    or RANGE, byte -1
    xor eax, eax
    mov STATE, eax
    inc eax
    lea edi, [ebp+REP_OFFSET(3)]
    push byte 4
    pop ecx
    rep stosd
    mov edi, ebp
    mov ecx, PROBS
    mov ah, (PROB_TOTAL / 2) >> 8
    dec eax
    rep stosw
    mov edi, OUTPUT
    xor ebx, ebx
    jmp next

error:
    mov esi, errmsg
    mov edi, 0xb8000
    mov ecx, errmsg.len
    mov ah, 0x4f
.print:
    lodsb
    stosw
    loop .print
    cli
.halt:
    hlt
    jmp .halt

normalise:
    cmp RANGE_HIGH, 0
    jne .done
    mov eax, SOURCE
    cmp eax, SOURCE_END
    jae error
    shl RANGE, 8
    shl CODE, 8
    mov al, [eax]
    mov CODE_LOW, al
    inc SOURCE
.done:
    ret

; ESI selects a probability; CF returns the bit. EAX is scratch.
bit:
    push edx
    cmp RANGE_HIGH, 0
    jne .ready
    call normalise
.ready:
    movzx eax, word [ebp+esi*2]
    mov edx, RANGE
    shr edx, PROB_BITS
    imul edx, eax
    cmp CODE, edx
    jb .zero
    sub CODE, edx
    sub RANGE, edx
    mov edx, eax
    shr edx, PROB_MOVE_BITS
    sub eax, edx
    stc
    jmp .store
.zero:
    mov RANGE, edx
    mov edx, PROB_TOTAL
    sub edx, eax
    shr edx, PROB_MOVE_BITS
    add eax, edx
    clc
.store:
    mov [ebp+esi*2], ax
    pop edx
    ret

next:
    cmp edi, OUTPUT_END
    je finish
    mov esi, STATE
    shl esi, POS_STATE_BITS
    xor ecx, ecx
    call bit
    jc match

    shr ebx, 8 - LITERAL_CONTEXT_BITS
    imul ecx, ebx, LITERAL_CONTEXT_SIZE
    add ecx, LIMLZ_LITERAL
    xor edx, edx
    mov ebx, 1
    mov eax, STATE
    cmp al, 7
    jb .literal_state
    mov edx, edi
    sub edx, REP0
    movzx edx, byte [edx]
    mov bh, 1
.literal_state:
    sub al, 3
    jae .nonzero_state
    xor eax, eax
.nonzero_state:
    cmp al, 7
    jb .set_state
    sub al, 3
.set_state:
    mov STATE, eax
.literal:
    shl edx, 1
    mov esi, ebx
    and esi, edx
    and esi, LITERAL_MATCH_BIT
    add esi, ebx
    add esi, ecx
    call bit
    adc bl, bl
    jc .literal_done
    mov eax, edx
    shr eax, 8
    xor al, bl
    test al, 1
    jz .literal
    mov bh, 0
    jmp .literal
.literal_done:
    mov [edi], bl
    inc edi
    movzx ebx, bl
    jmp next

match:
    push esi
    mov esi, STATE
    add esi, LIMLZ_IS_REP
    mov eax, STATE
    cmp al, 7
    sbb eax, eax
    and eax, -3
    add eax, 3
    mov STATE, eax
    call bit
    jc .repeat
    pop eax
    mov eax, REP0
    xchg eax, REP1
    xchg eax, REP2
    mov REP3, eax
    mov esi, LIMLZ_LENGTH
    call length
    push ecx
    add STATE, byte 7
    mov ebx, ecx
    sub ebx, MATCH_MIN
    cmp ebx, LENGTH_STATES - 1
    jbe .slot
    mov ebx, LENGTH_STATES - 1
.slot:
    shl ebx, SLOT_BITS
    add ebx, LIMLZ_SLOT
    push byte SLOT_BITS
    pop ecx
    call tree
    sub esi, 1 << SLOT_BITS
    mov ebx, esi
    cmp esi, SLOT_MODEL_START
    jb .distance
    mov ecx, esi
    shr ecx, 1
    dec ecx
    and ebx, 1
    or ebx, 2
    shl ebx, cl
    mov edx, ebx
    sub edx, esi
    add edx, LIMLZ_SPECIAL - 1
    cmp esi, SLOT_MODEL_END
    jb .reverse
.direct:
    dec ecx
    call normalise
    shr RANGE, 1
    mov eax, RANGE
    cmp CODE, eax
    jb .direct_zero
    sub CODE, eax
    bts ebx, ecx
.direct_zero:
    cmp ecx, ALIGN_BITS
    jne .direct
    mov edx, LIMLZ_ALIGN
.reverse:
    push byte 1
    pop esi
.reverse_bit:
    push esi
    add esi, edx
    call bit
    pop esi
    adc esi, esi
    dec ecx
    jnz .reverse_bit
.reverse_value:
    shr esi, 1
    jz .distance
    adc ecx, ecx
    jmp .reverse_value
.distance:
    add ebx, ecx
    inc ebx
    mov REP0, ebx
    pop ecx
    jmp copy
.repeat:
    add esi, LIMLZ_REP_G0 - LIMLZ_IS_REP
    call bit
    jc .other_rep
    pop esi
    add esi, LIMLZ_REP_LONG
    call bit
    jc .rep_length
    push byte 1
    pop ecx
    or STATE, byte 9
    jmp copy
.other_rep:
    pop eax
    add esi, LIMLZ_REP_G1 - LIMLZ_REP_G0
    call bit
    mov ebx, REP1
    jnc .rep1
    add esi, LIMLZ_REP_G2 - LIMLZ_REP_G1
    call bit
    mov ebx, REP2
    jnc .rep2
    xchg ebx, REP3
.rep2:
    mov eax, REP1
    mov REP2, eax
.rep1:
    mov eax, REP0
    mov REP1, eax
    mov REP0, ebx
.rep_length:
    mov esi, LIMLZ_REP_LENGTH
    call length
    or STATE, byte 8
copy:
    mov esi, edi
    sub esi, REP0
    jc invalid
    cmp esi, OUTPUT
    jb invalid
    cmp esi, edi
    jae invalid
    mov eax, OUTPUT_END
    sub eax, edi
    cmp ecx, eax
    ja invalid
    cmp REP0, byte 1
    jne .copy_match
    mov al, [esi]
    rep stosb
    jmp .copied
.copy_match:
    rep movsb
.copied:
    movzx ebx, byte [edi-1]
    jmp next

invalid:
    jmp error

; Decode a length at ESI for position ECX, returning ECX.
length:
    call bit
    lea ebx, [esi+ecx*LENGTH_LOW_SIZE+LENGTH_LOW]
    mov edx, MATCH_MIN - LENGTH_LOW_SIZE
    mov ecx, LENGTH_LOW_BITS
    jnc tree_length
    inc esi
    call bit
    lea ebx, [ebx+LENGTH_MID-LENGTH_LOW]
    mov edx, MATCH_MIN + LENGTH_LOW_SIZE - LENGTH_MID_SIZE
    jnc tree_length
    lea ebx, [esi+LENGTH_HIGH-1]
    mov edx, MATCH_MIN + LENGTH_LOW_SIZE + LENGTH_MID_SIZE - LENGTH_HIGH_SIZE
    mov cl, LENGTH_HIGH_BITS
tree_length:
    call tree
    lea ecx, [esi+edx]
    ret

; EBX is the model base, ECX the depth; ESI retains the leading tree bit.
tree:
    push byte 1
    pop esi
.loop:
    push esi
    add esi, ebx
    call bit
    pop esi
    adc esi, esi
    dec ecx
    jnz .loop
    ret

finish:
    call normalise
    cmp CODE, 0
    jne invalid
    mov eax, SOURCE
    cmp eax, SOURCE_END
    jne invalid
    mov esi, OUTPUT
    ; History prevents ambiguous transforms of overlapping E8/E9 operands.
    xor ebx, ebx
    lea edx, [esi-BCJ_OPERAND_SIZE]
.bcj_scan:
    lea ecx, [edi-BCJ_OPERAND_SIZE]
    cmp esi, ecx
    jae .bcj_done
    lodsb
    sub al, BCJ_CALL
    cmp al, 1
    ja .bcj_scan
    mov ecx, esi
    sub ecx, edx
    mov edx, esi
    cmp ecx, BCJ_OPERAND_SIZE
    jae .bcj_reset
.bcj_history:
    and bl, BCJ_HISTORY_MASK
    add bl, bl
    loop .bcj_history
    jmp .bcj_check
.bcj_reset:
    xor ebx, ebx
.bcj_check:
    mov al, [esi+BCJ_OPERAND_SIZE-1]
    inc al
    cmp al, 1
    ja .bcj_skip
    cmp bl, BCJ_MAX_HISTORY
    ja .bcj_skip
    cmp bl, BCJ_INVALID_HISTORY
    je .bcj_skip
    lea edx, [esi+BCJ_OPERAND_SIZE-OUTPUT]
    mov eax, [esi]
.bcj_adjust:
    sub eax, edx
    test bl, bl
    jz .bcj_store
    bsr ecx, ebx
    xor cl, 3
    shl ecx, 3
    push eax
    shr eax, cl
    inc al
    cmp al, 1
    pop eax
    ja .bcj_store
    push edx
    mov edx, -256
    shl edx, cl
    not edx
    xor eax, edx
    pop edx
    jmp .bcj_adjust
.bcj_store:
    shl eax, 32 - BCJ_SIGN_BITS
    sar eax, 32 - BCJ_SIGN_BITS
    mov [esi], eax
    mov edx, esi
    add esi, BCJ_OPERAND_SIZE
    xor ebx, ebx
    jmp .bcj_scan
.bcj_skip:
    or bl, 1
    cmp al, 1
    ja .bcj_scan
    or bl, BCJ_SIGN_HISTORY
    jmp .bcj_scan
.bcj_done:
    mov esi, OUTPUT
    or eax, byte -1
.crc_byte:
    cmp esi, edi
    je .crc_done
    xor al, [esi]
    inc esi
%rep 2
    mov edx, eax
    and edx, 15
    shr eax, 4
    xor eax, [crc_table+edx*4]
%endrep
    jmp .crc_byte
.crc_done:
    not eax
    cmp eax, CHECKSUM
    jne invalid
    lea esp, [ebp+PROBS*2]
    movzx eax, byte [esp+12]
    mov ecx, [esp+16]
    mov esp, OUTPUT
    xor ebp, ebp
    push ecx
    push eax
    push ebp
    push OUTPUT
    ret

errmsg: db "limine integrity error"
.len: equ $ - errmsg

; A nibble table keeps the CRC pass from dominating long match copies.
align 4
crc_table:
    dd 0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac
    dd 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c
    dd 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c
    dd 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
