#if defined(__x86_64__) || defined(__i386__)

#include <stdint.h>
#include <stddef.h>
#include <sys/idt.h>
#include <sys/cpu.h>
#include <sys/pic.h>
#include <sys/lapic.h>
#include <mm/pmm.h>
#include <lib/misc.h>

static struct idt_entry *dummy_idt = NULL;

void dummy_isr(void);

void init_flush_irqs(void) {
    size_t dummy_idt_size = 256 * sizeof(struct idt_entry);
    dummy_idt = ext_mem_alloc(dummy_idt_size);

    for (size_t i = 0; i < 256; i++) {
        dummy_idt[i].offset_lo = (uint16_t)(uintptr_t)dummy_isr;
        dummy_idt[i].type_attr = 0x8e;
#if defined(__i386__)
        dummy_idt[i].selector = 0x18;
        dummy_idt[i].offset_hi = (uint16_t)((uintptr_t)dummy_isr >> 16);
#elif defined(__x86_64__)
        dummy_idt[i].selector = 0x28;
        dummy_idt[i].offset_mid = (uint16_t)((uintptr_t)dummy_isr >> 16);
        dummy_idt[i].offset_hi = (uint32_t)((uintptr_t)dummy_isr >> 32);
#endif
    }
}

int irq_flush_type = IRQ_NO_FLUSH;

void flush_irqs(void) {
    switch (irq_flush_type) {
        case IRQ_PIC_ONLY_FLUSH:
            pic_flush();
            // FALLTHRU
        case IRQ_NO_FLUSH:
            return;
        case IRQ_PIC_APIC_FLUSH:
            break;
        default:
            panic(false, "Invalid IRQ flush type");
    }

    struct idtr old_idt;
    asm volatile ("sidt %0" : "=m"(old_idt) :: "memory");

    struct idtr new_idt = {
        256 * sizeof(struct idt_entry) - 1,
        (uintptr_t)dummy_idt
    };
    asm volatile ("lidt %0" :: "m"(new_idt) : "memory");

    // Flush the legacy PIC so we know the remaining ints come from the LAPIC
    pic_flush();

    asm volatile ("sti" ::: "memory");

    // Delay a while to make sure we catch ALL pending IRQs
    stall(10000);

    asm volatile ("cli" ::: "memory");

    asm volatile ("lidt %0" :: "m"(old_idt) : "memory");
}

#endif
