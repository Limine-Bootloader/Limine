// BIOS Limine's TSC calibration, common/sys/cpu.s2.c, run on a simulated
// machine where stalls, such as SMIs or a hypervisor taking the CPU away, land
// exactly where they hurt: inside a port access once the device has answered,
// or between accesses. Each scenario reports "K name=found wanted".

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
#define MS ((uint64_t)1000000000)

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
    // Added to the PM timer's count, to have it wrap where wanted.
    uint32_t pm_offset;
    // Chances, per million, of a stall inside a port access once the device
    // has answered, and of one before any access.
    uint32_t inside_ppm;
    uint32_t between_ppm;
    uint64_t seed;
    // PIT rounds, counted by control words, whose read first seeing OUT high
    // or whose write opening the gate stalls for EDGE_STALL inside.
    size_t edge_rounds;
    size_t gate_rounds;
};

#define EDGE_STALL ((uint64_t)8500000000)

static struct machine m;
static uint64_t now;
static uint64_t rng;

static uint8_t port61_low;
static size_t pit_round;
static uint16_t pit_count;
static size_t pit_count_bytes;
static bool pit_loaded;
static bool pit_gate;
// When OUT goes high, or UINT64_MAX while it is not counting towards that.
static uint64_t pit_out_at;
static bool pit_out;
static bool edge_seen;

static size_t port61_writes;
static size_t port61_reserved_writes;
static uint8_t port61_last;

static jmp_buf panicked;

static uint64_t scale(uint64_t value, uint64_t mul, uint64_t div) {
    return (uint64_t)((unsigned __int128)value * mul / div);
}

static uint64_t next_random(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static bool chance(uint32_t ppm) {
    return ppm != 0 && next_random() % 1000000 < ppm;
}

// From 10us to 8.5ms, most of them short, as SMIs are, and some as long as a
// host's timeslice.
static uint64_t stall_length(void) {
    static const uint64_t lengths[] = {
        10 * US, 30 * US, 100 * US, 300 * US, 1 * MS, 3 * MS, 8500 * US
    };
    uint64_t length = lengths[next_random() % SIZEOF_ARRAY(lengths)];
    return length + next_random() % (length / 2);
}

static void access_start(void) {
    if (chance(m.between_ppm)) {
        now += stall_length();
    }
    now += ACCESS / 2;
}

static void access_end(bool forced_stall) {
    if (forced_stall) {
        now += EDGE_STALL;
    } else if (chance(m.inside_ppm)) {
        now += stall_length();
    }
    now += ACCESS / 2;
}

static bool pit_out_now(void) {
    if (!pit_out && pit_out_at != UINT64_MAX && now >= pit_out_at) {
        pit_out = true;
    }
    return pit_out;
}

uint64_t rdtsc(void) {
    if (chance(m.between_ppm)) {
        now += stall_length();
    }
    uint64_t tsc = scale(now, m.tsc_hz, PS_PER_S);
    now += RDTSC_TIME;
    return tsc;
}

uint32_t ind(uint16_t port) {
    access_start();
    uint32_t value = 0xffffffff;
    if (port == PM_TIMER_PORT && m.pm_timer) {
        value = ((uint32_t)scale(now, PM_TIMER_HZ, PS_PER_S) + m.pm_offset) & 0xffffff;
    }
    access_end(false);
    return value;
}

uint8_t inb(uint16_t port) {
    access_start();
    uint8_t value = 0xff;
    bool edge = false;
    if (port == 0x61) {
        bool out = pit_out_now();
        value = (uint8_t)(port61_low | ((now / REFRESH_PERIOD) & 1) << 4 | (uint8_t)out << 5);
        edge = out && !edge_seen;
        if (out) {
            edge_seen = true;
        }
    }
    access_end(edge && pit_round != 0 && pit_round <= m.edge_rounds);
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
    bool gate_opened = false;
    switch (port) {
        case 0x61: {
            port61_writes++;
            port61_last = value;
            if ((value & 0xf0) != 0) {
                port61_reserved_writes++;
            }
            port61_low = value & 0x0f;
            bool gate = (value & 1) != 0;
            gate_opened = gate && !pit_gate;
            if (gate_opened && pit_loaded) {
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
                pit_round++;
                pit_count_bytes = 0;
                pit_loaded = false;
                pit_out = false;
                pit_out_at = UINT64_MAX;
                edge_seen = false;
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
    access_end(gate_opened && pit_round != 0 && pit_round <= m.gate_rounds);
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
    rng = m.seed * 0x9e3779b97f4a7c15 + 1;
    // Channel 2 left counted out, its gate open, and the NMI sources masked.
    port61_low = 0x0d;
    pit_round = 0;
    pit_loaded = false;
    pit_gate = true;
    pit_out = true;
    pit_out_at = UINT64_MAX;
    edge_seen = false;
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
    char name[64];

    printf("B TSC-SIM\n");
    run("pm.clean", (struct machine){ .tsc_hz = 2400000000, .pm_timer = true });
    run("pm.wrap", (struct machine){
        .tsc_hz = 3000000000, .pm_timer = true, .pm_offset = 0xffffff - 20000
    });
    for (uint64_t seed = 1; seed <= 32; seed++) {
        snprintf(name, sizeof(name), "pm.stalls.%" PRIu64, seed);
        run(name, (struct machine){
            .tsc_hz = 3878400000, .pm_timer = true, .inside_ppm = 100000,
            .between_ppm = 20000, .seed = seed
        });
    }

    run("pit.clean", (struct machine){ .tsc_hz = 2400000000 });
    for (uint64_t seed = 1; seed <= 32; seed++) {
        snprintf(name, sizeof(name), "pit.stalls.%" PRIu64, seed);
        run(name, (struct machine){
            .tsc_hz = 3878400000, .inside_ppm = 50, .between_ppm = 50, .seed = seed
        });
    }
    run("pit.edges", (struct machine){ .tsc_hz = 3878400000, .edge_rounds = 9 });
    run("pit.gates", (struct machine){ .tsc_hz = 3878400000, .gate_rounds = 9 });
    run("pit.all_edges", (struct machine){ .tsc_hz = 3878400000, .edge_rounds = 10 });

    printf("E DONE\n");
    return 0;
}
