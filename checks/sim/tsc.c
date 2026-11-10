// BIOS Limine's TSC calibration, common/sys/cpu.s2.c, run on a simulated
// machine: a TSC, an ACPI PM timer, PIT channel 2 and port 0x61. Each
// scenario reports "K name=found wanted".

#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <lib/acpi.h>
#include <lib/misc.h>
#include <sys/cpu.h>

#define PS_PER_S ((uint64_t)1000000000000)
#define US ((uint64_t)1000000)

// A port access takes ACCESS, the device answering halfway through.
#define ACCESS US
#define RDTSC_TIME ((uint64_t)20000)

#define PM_TIMER_PORT 0x608
#define PM_TIMER_HZ 3579545
#define PIT_HZ 1193182
// The refresh request toggle in port 0x61 bit 4.
#define REFRESH_PERIOD ((uint64_t)15085000)

struct machine {
    uint64_t tsc_hz;
    bool pm_timer;
};

static struct machine m;
static uint64_t now;

static uint8_t port61_low;
static uint16_t pit_count;
static size_t pit_count_bytes;
static bool pit_loaded;
static bool pit_gate;
// When OUT goes high, or UINT64_MAX while it is not counting towards that.
static uint64_t pit_out_at;
static bool pit_out;

static size_t port61_writes;
static size_t port61_reserved_writes;
static uint8_t port61_last;

static jmp_buf panicked;

static uint64_t scale(uint64_t value, uint64_t mul, uint64_t div) {
    return (uint64_t)((unsigned __int128)value * mul / div);
}

static void access_start(void) {
    now += ACCESS / 2;
}

static void access_end(void) {
    now += ACCESS / 2;
}

static bool pit_out_now(void) {
    if (!pit_out && pit_out_at != UINT64_MAX && now >= pit_out_at) {
        pit_out = true;
    }
    return pit_out;
}

uint64_t rdtsc(void) {
    uint64_t tsc = scale(now, m.tsc_hz, PS_PER_S);
    now += RDTSC_TIME;
    return tsc;
}

uint32_t ind(uint16_t port) {
    access_start();
    uint32_t value = 0xffffffff;
    if (port == PM_TIMER_PORT && m.pm_timer) {
        value = (uint32_t)scale(now, PM_TIMER_HZ, PS_PER_S) & 0xffffff;
    }
    access_end();
    return value;
}

uint8_t inb(uint16_t port) {
    access_start();
    uint8_t value = 0xff;
    if (port == 0x61) {
        bool out = pit_out_now();
        value = (uint8_t)(port61_low | ((now / REFRESH_PERIOD) & 1) << 4 | (uint8_t)out << 5);
    }
    access_end();
    return value;
}

// Mode 0: the count goes down once a PIT clock while the gate is high, from
// the first clock after it rises, and OUT goes high when it reaches 0.
static void pit_start_counting(void) {
    uint64_t first_clock = scale(now, PIT_HZ, PS_PER_S) + 1;
    pit_out_at = scale(first_clock + pit_count - 1, PS_PER_S, PIT_HZ) + 1;
}

void outb(uint16_t port, uint8_t value) {
    access_start();
    switch (port) {
        case 0x61: {
            port61_writes++;
            port61_last = value;
            if ((value & 0xf0) != 0) {
                port61_reserved_writes++;
            }
            port61_low = value & 0x0f;
            bool gate = (value & 1) != 0;
            if (gate && !pit_gate && pit_loaded) {
                pit_start_counting();
            } else if (!gate && pit_out_at != UINT64_MAX && !pit_out_now()) {
                // Not modelled: counting resumed after a pause.
                pit_out_at = UINT64_MAX;
            }
            pit_gate = gate;
            break;
        }
        case 0x43: {
            if (value == 0xb0) {
                pit_count_bytes = 0;
                pit_loaded = false;
                pit_out = false;
                pit_out_at = UINT64_MAX;
            }
            break;
        }
        case 0x42: {
            if (pit_count_bytes == 0) {
                pit_count = value;
            } else {
                pit_count |= (uint16_t)(value << 8);
                pit_loaded = true;
                if (pit_gate) {
                    pit_start_counting();
                }
            }
            pit_count_bytes++;
            break;
        }
    }
    access_end();
}

// A processor whose CPUID leaves stop short of 0x15.
bool cpuid(
    uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx
) {
    (void)subleaf;
    if (leaf > 0xd) {
        return false;
    }
    *eax = leaf == 0 ? 0xd : 0;
    *ebx = *ecx = *edx = 0;
    return true;
}

// As sys/cpu.h has it on x86.
uint64_t tsc_freq_arch(void) {
    uint32_t eax, ebx, ecx, edx;
    if (!cpuid(0x15, 0, &eax, &ebx, &ecx, &edx) || eax == 0 || ebx == 0 || ecx == 0) {
        return 0;
    }
    return (uint64_t)ecx * ebx / eax;
}

// A FADT of ACPI 6.6 5.2.9's size, with a 24-bit PM timer.
static uint8_t fadt[276];

void *acpi_get_table_quiet(const char *signature, int index) {
    if (!m.pm_timer || index != 0 || strcmp(signature, "FACP") != 0) {
        return NULL;
    }
    uint32_t length = sizeof(fadt);
    uint32_t block = PM_TIMER_PORT;
    memset(fadt, 0, sizeof(fadt));
    memcpy(fadt, "FACP", 4);
    memcpy(fadt + 4, &length, 4);
    memcpy(fadt + 76, &block, 4);
    fadt[91] = 4;
    return fadt;
}

noreturn void panic(bool allow_menu, const char *fmt, ...) {
    (void)allow_menu;
    va_list args;
    va_start(args, fmt);
    printf("K panic=");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
    longjmp(panicked, 1);
}

static void run(const char *name, struct machine machine) {
    m = machine;
    now = 0;
    // Channel 2 left counted out, its gate open, and the NMI sources masked.
    port61_low = 0x0d;
    pit_loaded = false;
    pit_gate = true;
    pit_out = true;
    pit_out_at = UINT64_MAX;
    port61_writes = 0;
    port61_reserved_writes = 0;
    port61_last = 0;

    tsc_freq = 0;
    if (setjmp(panicked) == 0) {
        calibrate_tsc();
    }
    printf("K %s=%" PRIu64 " %" PRIu64 "\n", name, tsc_freq, m.tsc_hz);
    if (port61_writes != 0) {
        printf(
            "K %s.port61=%zu %zu %02x\n", name, port61_writes, port61_reserved_writes, port61_last
        );
    }
}

int main(void) {
    printf("B TSC-SIM\n");
    run("pit.clean", (struct machine){ .tsc_hz = 2400000000 });

    printf("E DONE\n");
    return 0;
}
