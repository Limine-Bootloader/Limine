#if defined (BIOS)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <lib/acpi.h>
#include <lib/misc.h>
#include <drivers/serial.h>
#include <sys/cpu.h>
#include <mm/pmm.h>

static bool serial_initialised = false;
// Set once a port has been found. serial is cleared when the search fails and a
// COM_OUTPUT build does not consult it, so it cannot stand for "a port exists".
static bool serial_present = false;
// Kept apart from serial, which also selects the menu's ASCII line drawing
// and gates serial input: a stalled transmitter must change neither.
static bool serial_stalled = false;
static bool serial_mmio;
static uintptr_t serial_base;
// The clock SPCR states, or zero where the table carries none.
static uint32_t serial_clock;
uint32_t serial_baudrate = 115200;

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
    uint8_t interrupt_type;
    uint8_t irq;
    uint32_t global_system_interrupt;
    uint8_t configured_baud_rate;
    uint8_t parity;
    uint8_t stop_bits;
    uint8_t flow_control;
    uint8_t terminal_type;
    uint8_t language;
    uint16_t pci_device_id;
    uint16_t pci_vendor_id;
    uint8_t pci_bus_number;
    uint8_t pci_device_number;
    uint8_t pci_function_number;
    uint32_t pci_flags;
    uint8_t pci_segment;
    uint32_t uart_clock_frequency;
} __attribute__((packed));

// Derived from what serial_find() reads below it rather than from the
// structure's size: those fields end at the base address.
#define SPCR_MIN_LENGTH offsetof(struct acpi_spcr, interrupt_type)

// Pinned to the field rather than to the structure: a later revision's fields
// would grow sizeof past the length every revision 3 table declares.
#define SPCR_CLOCK_LENGTH \
    (offsetof(struct acpi_spcr, uart_clock_frequency) + sizeof(uint32_t))

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

// A port nothing decodes floats to 0xff on every read, which serial_in() takes
// for a keystroke. The divisor latch is what a real UART has to give back.
static bool serial_probe(void) {
    serial_write(3, 0x80);
    serial_write(0, 0xa5);
    serial_write(1, 0x5a);
    bool answered = serial_read(0) == 0xa5 && serial_read(1) == 0x5a;
    serial_write(3, 0x00);
    return answered;
}

// Runs from serial_out(), so nothing reached from here may print: a diagnostic
// would re-enter serial_out() and recurse. Hence acpi_get_table_quiet() below.
static bool serial_find(void) {
    uint16_t bda_port = mminw(0x400);
    // Ports below 0x100 are fixed motherboard registers, and serial_probe()
    // writes the port before anything has confirmed a UART is there.
    if (bda_port >= 0x100 && bda_port <= UINT16_MAX - 7) {
        serial_base = bda_port;
        serial_mmio = false;
        // A candidate that does not answer rules out the candidate rather
        // than the search, so a stale BDA word still reaches the SPCR below.
        if (serial_probe()) {
            return true;
        }
    }

    struct acpi_spcr *spcr = acpi_get_table_quiet("SPCR", 0);
    if (spcr == NULL || spcr->header.length < SPCR_MIN_LENGTH
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

    uintptr_t base = (uintptr_t)spcr->base_address.address;
    bool mmio;
    // No UART aperture is in the first megabyte, so a base there is an I/O
    // port the table put in the wrong space, and programming it writes RAM.
    if (spcr->base_address.address_space == 0
     && base >= 0x100000) {
        mmio = true;
    } else if (spcr->base_address.address_space == 1
            && base >= 0x100 && base <= UINT16_MAX - 7) {
        mmio = false;
    } else {
        return false;
    }

    // A divisor-latch probe cannot tell an aperture from RAM, which gives the
    // written value straight back, so the memory map is what has to say.
    if (mmio) {
        uint64_t base_type = pmm_check_type(base);
        if (base_type == MEMMAP_USABLE
         || base_type == MEMMAP_BOOTLOADER_RECLAIMABLE) {
            return false;
        }
    }

    serial_base = base;
    serial_mmio = mmio;

    if (!serial_probe()) {
        return false;
    }

    if (spcr->header.rev >= 3
     && spcr->header.length >= SPCR_CLOCK_LENGTH) {
        serial_clock = spcr->uart_clock_frequency;
    }

    return true;
}

static void serial_initialise(void) {
    // COM_OUTPUT is a build-time output channel and does not depend on the key.
    if (serial_initialised || (!serial && !COM_OUTPUT)) {
        return;
    }

    if (!serial_find()) {
        // serial also picks the menu's row budget, read before this search, and
        // its glyphs, read after: so the glyphs are right and the rows are not.
        serial = false;
        serial_initialised = true;
        return;
    }

    serial_write(3, 0x00);
    serial_write(1, 0x00);
    serial_write(3, 0x80);

    // SPCR states the clock from revision 3; 1.8432 MHz is the ISA part's,
    // which is the only one an older table can be describing.
    uint32_t uart_clock = serial_clock != 0 ? serial_clock : 1843200;
    uint32_t divisor = uart_clock / (16 * serial_baudrate);
    if (divisor == 0) {
        divisor = 1;
    } else if (divisor > UINT16_MAX) {
        divisor = UINT16_MAX;
    }
    serial_write(0, divisor & 0xff);
    serial_write(1, (divisor >> 8) & 0xff);

    serial_write(3, 0x03);
    serial_write(2, 0xc7);
    serial_write(4, 0x0b);

    serial_initialised = true;
    serial_present = true;
}

void serial_out(uint8_t b) {
    serial_initialise();

    if (!serial_present) {
        return;
    }

    if (serial_stalled) {
        // A stall can be transient, so retry without paying the wait again.
        if ((serial_read(5) & 0x20) == 0) {
            return;
        }
        serial_stalled = false;
    }

    // Ten bit times per character, so both bounds have to follow the rate. The
    // config clamps the key against 0 and 115200 only, so the rate is floored
    // here at the slowest standard one to keep both bounds finite.
    uint32_t rate = serial_baudrate < 50 ? 50 : serial_baudrate;
    uint64_t wait_us = (uint64_t)10000000 * 16 / rate;
    if (wait_us < 100000) {
        wait_us = 100000;
    }
    uint64_t deadline = rdtsc_deadline(wait_us);
    // With no TSC there is no clock to wait against, so bound by poll count.
    size_t retries = 10000 * (rate < 115200 ? 115200 / rate : 1);

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

    // Input stays keyed to the config; serial_present is what the read needs.
    if (!serial || !serial_present || (serial_read(5) & 0x01) == 0) {
        return -1;
    }
    return serial_read(0);
}

#endif
