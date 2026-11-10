#ifndef SYS__CPU_H
#define SYS__CPU_H

#include <stdbool.h>
#include <stdint.h>

// What cpu.s2.c reaches of the hardware, as tsc.c simulates it.
uint64_t rdtsc(void);
uint8_t inb(uint16_t port);
uint32_t ind(uint16_t port);
void outb(uint16_t port, uint8_t value);
bool cpuid(
    uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx
);
uint64_t tsc_freq_arch(void);

extern uint64_t tsc_freq;
void calibrate_tsc(void);

#endif
