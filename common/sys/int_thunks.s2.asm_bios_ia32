section .text

extern except

%macro raise_exception 1
align 16
raise_exception_%1:
    cld
%if %1 == 8 || %1 == 10 || %1 == 11 || %1 == 12 || %1 == 13 || %1 == 14 || %1 == 17 || %1 == 30
    pop eax
%else
    xor eax, eax
%endif
    push ebp
    mov ebp, esp
    push eax
    push %1
    call except
%endmacro

%assign i 0
%rep 32
raise_exception i
%assign i i+1
%endrep

section .rodata

%macro raise_exception_getaddr 1
dd raise_exception_%1
%endmacro

global exceptions
exceptions:
%assign i 0
%rep 32
raise_exception_getaddr i
%assign i i+1
%endrep
