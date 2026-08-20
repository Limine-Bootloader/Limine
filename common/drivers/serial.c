#if defined (BIOS)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <lib/acpi.h>
#include <lib/misc.h>
#include <drivers/serial.h>
#include <sys/cpu.h>

static bool serial_initialised = false;
// Kept apart from serial, which also selects the menu's ASCII line drawing
// and gates serial input: a stalled transmitter must change neither.
static bool serial_stalled = false;
static bool serial_mmio;
static uintptr_t serial_base;
uint32_t serial_baudrate;

struct acpi_gas {
    uint8_t address_space;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed));

struct acpi_spcr {
    struct sdt header;
    uint8_t interface_type;
    uint8_t reserved[3];
    struct acpi_gas base_address;
} __attribute__((packed));

static uint8_t serial_read(size_t reg) {
    if (serial_mmio) {
        return *(volatile uint8_t *)(serial_base + reg);
    }
    return inb(serial_base + reg);
}

static void serial_write(size_t reg, uint8_t value) {
    if (serial_mmio) {
        *(volatile uint8_t *)(serial_base + reg) = value;
        return;
    }
    outb(serial_base + reg, value);
}

// Runs from serial_out(), so nothing reached from here may print: a diagnostic
// would re-enter serial_out() and recurse. Hence acpi_get_table_quiet() below.
static bool serial_find(void) {
    uint16_t bda_port = mminw(0x400);
    if (bda_port != 0) {
        serial_base = bda_port;
        serial_mmio = false;
        return true;
    }

    struct acpi_spcr *spcr = acpi_get_table_quiet("SPCR", 0);
    if (spcr == NULL || spcr->header.length < sizeof(struct acpi_spcr)
     || acpi_checksum(spcr, spcr->header.length) != 0) {
        return false;
    }

    if (spcr->interface_type != 0 && spcr->interface_type != 1
     && (spcr->header.rev < 2 || spcr->interface_type != 0x12)) {
        return false;
    }

    if (spcr->base_address.bit_width != 8
     || spcr->base_address.bit_offset != 0
     || (spcr->base_address.access_size != 0
      && spcr->base_address.access_size != 1)
     || spcr->base_address.address == 0
     || spcr->base_address.address > UINTPTR_MAX - 7) {
        return false;
    }

    serial_base = (uintptr_t)spcr->base_address.address;
    if (spcr->base_address.address_space == 0) {
        serial_mmio = true;
    } else if (spcr->base_address.address_space == 1
            && serial_base <= UINT16_MAX - 7) {
        serial_mmio = false;
    } else {
        return false;
    }

    return true;
}

static void serial_initialise(void) {
    if (serial_initialised || !serial) {
        return;
    }

    if (!serial_find()) {
        serial = false;
        serial_initialised = true;
        return;
    }

    serial_write(3, 0x00);
    serial_write(1, 0x00);
    serial_write(3, 0x80);

    uint16_t divisor = (uint16_t)(115200 / serial_baudrate);
    serial_write(0, divisor & 0xff);
    serial_write(1, (divisor >> 8) & 0xff);

    serial_write(3, 0x03);
    serial_write(2, 0xc7);
    serial_write(4, 0x0b);

    serial_initialised = true;
}

void serial_out(uint8_t b) {
    serial_initialise();

    if (!serial || serial_stalled) {
        return;
    }

    uint64_t deadline = rdtsc_deadline(100000);
    // With no TSC there is no clock to wait against, so bound by poll count
    // instead, comfortably above the 87 us a character takes at 115200 baud.
    size_t retries = 10000;

    while ((serial_read(5) & 0x20) == 0) {
        if (deadline != 0 && !rdtsc_deadline_expired(deadline)) {
            continue;
        }
        if (deadline == 0 && --retries != 0) {
            continue;
        }

        serial_stalled = true;
        return;
    }
    serial_write(0, b);
}

int serial_in(void) {
    serial_initialise();

    if (!serial || (serial_read(5) & 0x01) == 0) {
        return -1;
    }
    return serial_read(0);
}

#endif
