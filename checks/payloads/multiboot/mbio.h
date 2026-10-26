#ifndef MBIO_H
#define MBIO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void putc_(char c);
void puts_(const char *s);
void hex(uint64_t v);
void fact(const char *key, uint64_t value);
void fact_str(const char *key, const char *value, size_t max);
void fact_hexdump(const char *key, const volatile void *data, size_t len);
uint64_t fnv1a(const volatile void *data, uint64_t len);

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    asm volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t v) {
    asm volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return lo | ((uint64_t)hi << 32);
}

#endif
