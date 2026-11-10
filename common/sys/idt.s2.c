#include <stddef.h>
#include <stdint.h>
#include <sys/idt.h>
#include <lib/misc.h>

#if defined(BIOS)

static struct idt_entry idt_entries[32];

__attribute__((section(".realmode")))
struct idtr idt = {
    sizeof(idt_entries) - 1,
    (uintptr_t)idt_entries
};

static void register_interrupt_handler(size_t vec, void *handler, uint8_t type) {
    uint32_t p = (uint32_t)handler;

    idt_entries[vec].offset_lo = (uint16_t)p;
    idt_entries[vec].selector = 0x18;
    idt_entries[vec].unused = 0;
    idt_entries[vec].type_attr = type;
    idt_entries[vec].offset_hi = (uint16_t)(p >> 16);
}

extern void *exceptions[];

void init_idt(void) {
    for (size_t i = 0; i < SIZEOF_ARRAY(idt_entries); i++)
        register_interrupt_handler(i, exceptions[i], 0x8e);

    asm volatile ("lidt %0" :: "m"(idt) : "memory");
}

#endif
