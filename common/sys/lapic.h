#ifndef SYS__APIC_H__
#define SYS__APIC_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct madt;

#define LAPIC_REG_ICR0     0x300
#define LAPIC_REG_ICR1     0x310
#define LAPIC_REG_SPURIOUS 0x0f0
#define LAPIC_REG_EOI      0x0b0
#define LAPIC_REG_ID       0x020

bool lapic_check(void);
void lapic_eoi(void);
uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t data);
void lapic_icr_wait(void);

bool x2apic_check(void);
bool x2apic_enable(void);
bool x2apic_disable(void);
uint64_t x2apic_read(uint32_t reg);
void x2apic_write(uint32_t reg, uint64_t data);

void lapic_configure_bsp(void);
void lapic_prep_lint(struct madt *madt, uint32_t acpi_uid);
void lapic_configure_handoff_state(void);

void init_io_apics(void);
uint32_t io_apic_read(size_t io_apic, uint32_t reg);
void io_apic_write(size_t io_apic, uint32_t reg, uint32_t value);
uint32_t io_apic_gsi_count(size_t io_apic);
void io_apic_mask_all(bool mask_nmi_and_extint);

#endif
