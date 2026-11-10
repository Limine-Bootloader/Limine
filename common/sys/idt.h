#ifndef SYS__IDT_H
#define SYS__IDT_H

#include <stdint.h>

#if defined(__i386__)

struct idtr {
    uint16_t limit;
    uint32_t ptr;
} __attribute__((packed));

struct idt_entry {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  unused;
    uint8_t  type_attr;
    uint16_t offset_hi;
} __attribute__((packed));

#elif defined(__x86_64__)

struct idtr {
    uint16_t limit;
    uint64_t ptr;
} __attribute__((packed));

struct idt_entry {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t reserved;
} __attribute__((packed));

#endif

#if defined(BIOS)

extern struct idtr idt;

void init_idt(void);

#endif

enum {
    IRQ_NO_FLUSH,
    IRQ_PIC_ONLY_FLUSH,
    IRQ_PIC_APIC_FLUSH
};

extern int irq_flush_type;

void init_flush_irqs(void);
void flush_irqs(void);

#endif
