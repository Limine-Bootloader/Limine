#if defined (__x86_64__) && defined (UEFI)

#include <lib/trueos_firmware_bridge.h>

    .section .text.trueos_firmware_bridge,"ax",@progbits
    .balign 4096
    .global trueos_firmware_bridge_entry
    .type trueos_firmware_bridge_entry, @function
trueos_firmware_bridge_entry:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushfq
    cli

    movq %rdi, %r12
    movq %cr3, %rax
    movq %rax, TRUEOS_FW_BRIDGE_OFF_CALLER_CR3(%r12)
    movq %rsp, TRUEOS_FW_BRIDGE_OFF_CALLER_RSP(%r12)
    movq %cr4, %r13
    movq %r13, TRUEOS_FW_BRIDGE_OFF_CALLER_CR4(%r12)

    /* Drop PGE while crossing address spaces so stale global translations
       cannot survive the CR3 change. TRUEOS restores the exact caller CR4. */
    movq %r13, %rax
    andq $-129, %rax
    movq %rax, %cr4

    movq TRUEOS_FW_BRIDGE_OFF_FIRMWARE_CR3(%r12), %rax
    movq %rax, %cr3
    movq TRUEOS_FW_BRIDGE_OFF_FIRMWARE_STACK_TOP(%r12), %rsp
    andq $-16, %rsp
    subq $32, %rsp

    movq TRUEOS_FW_BRIDGE_OFF_ARG0(%r12), %rcx
    movq TRUEOS_FW_BRIDGE_OFF_ARG1(%r12), %rdx
    movq TRUEOS_FW_BRIDGE_OFF_ARG2(%r12), %r8
    movq TRUEOS_FW_BRIDGE_OFF_ARG3(%r12), %r9
    movq TRUEOS_FW_BRIDGE_OFF_TARGET(%r12), %rax
    call *%rax
    movq %rax, TRUEOS_FW_BRIDGE_OFF_RESULT(%r12)

    cli
    movq TRUEOS_FW_BRIDGE_OFF_CALLER_CR3(%r12), %rax
    movq %rax, %cr3
    movq TRUEOS_FW_BRIDGE_OFF_CALLER_CR4(%r12), %rax
    movq %rax, %cr4
    movq TRUEOS_FW_BRIDGE_OFF_CALLER_RSP(%r12), %rsp

    popfq
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret

    .size trueos_firmware_bridge_entry, .-trueos_firmware_bridge_entry
    .global trueos_firmware_bridge_end
trueos_firmware_bridge_end:
    .balign 4096

#endif
