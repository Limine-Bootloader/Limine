#ifndef SYS__GDT_H
#define SYS__GDT_H

#include <stdint.h>

struct gdtr {
    uint16_t limit;
#if defined(__x86_64__)
    uint64_t ptr;
#elif defined(__i386__)
    uint32_t ptr;
    uint32_t ptr_hi;
#endif
} __attribute__((packed));

struct gdt_desc {
    uint16_t limit;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_hi;
} __attribute__((packed));

extern struct gdtr gdt;

void init_gdt(void);

#endif
