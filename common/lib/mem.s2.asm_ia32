section .text

global memcpy
memcpy:
    push esi
    push edi
    mov eax, dword [esp+12]
    mov edi, eax
    mov esi, dword [esp+16]
    mov ecx, dword [esp+20]
    rep movsb
    pop edi
    pop esi
    ret

global memset
memset:
    push edi
    mov edx, dword [esp+8]
    mov edi, edx
    mov eax, dword [esp+12]
    mov ecx, dword [esp+16]
    rep stosb
    mov eax, edx
    pop edi
    ret

global memmove
memmove:
    push esi
    push edi
    mov eax, dword [esp+12]
    mov edi, eax
    mov esi, dword [esp+16]
    mov ecx, dword [esp+20]

    cmp edi, esi
    ja .copy_backwards

    rep movsb
    jmp .done

  .copy_backwards:
    lea edi, [edi+ecx-1]
    lea esi, [esi+ecx-1]
    std
    rep movsb
    cld

  .done:
    pop edi
    pop esi
    ret

global memcmp
memcmp:
    push esi
    push edi
    mov edi, dword [esp+12]
    mov esi, dword [esp+16]
    mov ecx, dword [esp+20]
    repe cmpsb
    je .equal
    mov al, byte [edi-1]
    sub al, byte [esi-1]
    movsx eax, al
    jmp .done

  .equal:
    xor eax, eax

  .done:
    pop edi
    pop esi
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
