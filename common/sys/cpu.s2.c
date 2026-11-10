#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/cpu.h>
#include <lib/misc.h>
#if defined(BIOS)
#include <lib/acpi.h>
#include <lib/libc.h>
#endif
#if defined(UEFI)
#include <efi.h>
#endif

uint64_t tsc_freq = 0;

#if defined(BIOS)

// FADT offsets, ACPI 6.6 5.2.9. PM_TMR_LEN is 4 where the timer exists and 0
// where it does not; TMR_VAL_EXT in the flags is set where the count is 32
// bits wide rather than 24.
#define FADT_PM_TMR_BLK 76
#define FADT_PM_TMR_LEN 91
#define FADT_FLAGS 112
#define FADT_X_PM_TMR_BLK 208
#define FADT_TMR_VAL_EXT ((uint32_t)1 << 8)

// ACPI 6.6 4.8.2.1: free-running, count-up, and at this fixed frequency.
#define PM_TIMER_FREQ 3579545
#define PM_TIMER_CALIBRATION_TICKS (PM_TIMER_FREQ / 100)
#define PM_TIMER_CALIBRATION_ROUNDS 3
#define PM_TIMER_START_READS 8

// A block that decodes to nothing reads a constant, so the count never
// arrives. The poll bound is sized for the fastest port access; a timer that
// is not counting is caught by the stall bound, which rests on its period.
#define PM_TIMER_MAX_POLLS 1000000
#define PM_TIMER_MAX_STALLED_POLLS 1000

static bool pm_timer_find(uint16_t *port, uint32_t *mask) {
    uint8_t *fadt = acpi_get_table_quiet("FACP", 0);
    if (fadt == NULL) {
        return false;
    }

    uint32_t fadt_length;
    memcpy(&fadt_length, fadt + 4, sizeof(fadt_length));

    if (fadt_length < FADT_PM_TMR_LEN + 1 || fadt[FADT_PM_TMR_LEN] != 4) {
        return false;
    }

    uint64_t address = 0;
    if (fadt_length >= FADT_X_PM_TMR_BLK + 12) {
        uint64_t x_address;
        memcpy(&x_address, fadt + FADT_X_PM_TMR_BLK + 4, sizeof(x_address));
        // The extended block supersedes the 32-bit one only where OSPM can use
        // it, and this reads a port rather than an aperture.
        if (x_address != 0 && fadt[FADT_X_PM_TMR_BLK] == 1) {
            address = x_address;
        }
    }
    if (address == 0 && fadt_length >= FADT_PM_TMR_BLK + 4) {
        uint32_t blk;
        memcpy(&blk, fadt + FADT_PM_TMR_BLK, sizeof(blk));
        address = blk;
    }

    // A 32-bit read reaches four ports, and below 0x100 they are fixed
    // motherboard registers: a base of 0x60 would pop the i8042's buffer.
    if (address < 0x100 || address > UINT16_MAX - 3) {
        return false;
    }

    uint32_t flags = 0;
    if (fadt_length >= FADT_FLAGS + 4) {
        memcpy(&flags, fadt + FADT_FLAGS, sizeof(flags));
    }

    *port = (uint16_t)address;
    *mask = (flags & FADT_TMR_VAL_EXT) != 0 ? 0xffffffff : 0x00ffffff;
    return true;
}

// A stall inside an access, for an SMI or a hypervisor, shows as a slow one,
// which cannot be placed in time. A stall between accesses is counted by both
// clocks and cancels out.
static uint32_t pm_timer_read(uint16_t port, uint32_t mask, uint64_t *tsc, uint64_t *cost) {
    uint64_t before = rdtsc();
    uint32_t count = ind(port) & mask;
    uint64_t after = rdtsc();

    *tsc = before + (after - before) / 2;
    *cost = after - before;
    return count;
}

static uint64_t calibrate_tsc_pm_timer(void) {
    uint16_t port;
    uint32_t mask;
    if (!pm_timer_find(&port, &mask)) {
        return 0;
    }

    uint64_t quickest = UINT64_MAX;
    uint64_t best_delta = 0;
    uint32_t best_elapsed = 0;

    for (int round = 0; round < PM_TIMER_CALIBRATION_ROUNDS; round++) {
        uint32_t start = 0;
        uint64_t tsc_start = 0;
        uint64_t start_cost = UINT64_MAX;
        for (int i = 0; i < PM_TIMER_START_READS; i++) {
            uint64_t tsc, cost;
            uint32_t count = pm_timer_read(port, mask, &tsc, &cost);
            if (cost < start_cost) {
                start = count;
                tsc_start = tsc;
                start_cost = cost;
            }
        }
        if (start_cost < quickest) {
            quickest = start_cost;
        }

        // The first read past the count that is itself quick ends the round,
        // or failing one by twice the count, the quickest read past it.
        uint32_t end_elapsed = 0;
        uint64_t tsc_end = 0;
        uint64_t end_cost = UINT64_MAX;
        uint32_t seen = 0;
        size_t stalled = 0;
        for (size_t polls = 0; polls < PM_TIMER_MAX_POLLS; polls++) {
            uint64_t tsc, cost;
            uint32_t elapsed = (pm_timer_read(port, mask, &tsc, &cost) - start) & mask;
            if (cost < quickest) {
                quickest = cost;
            }
            if (elapsed >= PM_TIMER_CALIBRATION_TICKS && cost < end_cost) {
                end_elapsed = elapsed;
                tsc_end = tsc;
                end_cost = cost;
                if (cost <= quickest * 2) {
                    break;
                }
            }
            if (elapsed >= PM_TIMER_CALIBRATION_TICKS * 2) {
                break;
            }
            if (elapsed != seen) {
                seen = elapsed;
                stalled = 0;
            } else if (++stalled >= PM_TIMER_MAX_STALLED_POLLS) {
                break;
            }
        }

        if (end_cost == UINT64_MAX) {
            return 0;
        }

        if (tsc_end > tsc_start) {
            uint64_t delta = tsc_end - tsc_start;
            if (delta < best_delta || best_delta == 0) {
                best_delta = delta;
                best_elapsed = end_elapsed;
            }
        }
    }

    if (best_delta == 0) {
        return 0;
    }

    // The count reached rather than the count asked for: the poll that ends
    // the wait overshoots by however long a port access takes.
    return best_delta * PM_TIMER_FREQ / best_elapsed;
}

#define PIT_FREQ 1193182
// About 10ms of channel 2 counting down.
#define PIT_CALIBRATION_COUNT 11932
#define PIT_CALIBRATION_MIN_ROUNDS 3
#define PIT_CALIBRATION_MAX_ROUNDS 10

// A gate that never asserts leaves OUT low, so the wait never ends. The
// count is sized for the fastest port access, which is what decides how
// many reads 10ms takes; on a slow one it is what the give-up costs.
#define PIT_MAX_POLLS 1000000

static uint64_t calibrate_tsc_pit(void) {
    // Bits 7:4 are read-only, and written as 0. Intel ICH10 319973-003,
    // 13.7.1.
    uint8_t port61 = inb(0x61) & 0x0f;

    uint64_t quickest = UINT64_MAX;
    uint64_t best_delta = 0;
    // From after the gate write to after the read that sees OUT high, for
    // when no round is clean.
    uint64_t best_plain_delta = 0;

    for (int round = 0; round < PIT_CALIBRATION_MAX_ROUNDS; round++) {
        if (round >= PIT_CALIBRATION_MIN_ROUNDS && best_delta != 0) {
            break;
        }

        outb(0x61, port61 & 0x0c); // disable gate and speaker
        outb(0x43, 0xb0); // channel 2, lobyte/hibyte, mode 0, binary
        outb(0x42, PIT_CALIBRATION_COUNT & 0xff);
        outb(0x42, (PIT_CALIBRATION_COUNT >> 8) & 0xff);

        // Mode 0 drives OUT low on the control word and raises it only at
        // terminal count, so a port already high is not a counting timer.
        if ((inb(0x61) & 0x20) != 0) {
            continue;
        }

        uint64_t gate_before = rdtsc();
        outb(0x61, (port61 & 0x0c) | 0x01); // enable gate to start counting
        uint64_t gate_after = rdtsc();
        if (gate_after - gate_before < quickest) {
            quickest = gate_after - gate_before;
        }

        // OUT went high within the last two reads.
        uint64_t prev_before = gate_before;
        uint64_t before = gate_before;
        uint64_t after = gate_after;
        bool counted = false;
        for (size_t polls = 0; polls < PIT_MAX_POLLS; polls++) {
            prev_before = before;
            before = rdtsc();
            uint8_t status = inb(0x61);
            after = rdtsc();
            if (after - before < quickest) {
                quickest = after - before;
            }
            if ((status & 0x20) != 0) {
                counted = true;
                break;
            }
        }

        if (!counted) {
            break;
        }

        if (after > gate_after) {
            uint64_t delta = after - gate_after;
            if (delta < best_plain_delta || best_plain_delta == 0) {
                best_plain_delta = delta;
            }
        }

        // Unlike a PM timer read, the edge comes once a round, so a stall that
        // widens either bracket costs the round.
        uint64_t start = gate_before + (gate_after - gate_before) / 2;
        uint64_t end = prev_before + (after - prev_before) / 2;
        if (gate_after - gate_before <= quickest * 2 && after - prev_before <= quickest * 4
         && end > start) {
            uint64_t delta = end - start;
            if (delta < best_delta || best_delta == 0) {
                best_delta = delta;
            }
        }
    }

    outb(0x61, port61); // restore

    uint64_t delta = best_delta != 0 ? best_delta : best_plain_delta;
    return delta * PIT_FREQ / PIT_CALIBRATION_COUNT;
}

#endif

#if defined(__x86_64__) || defined(__i386__)
// For where CPUID leaf 0x15 gives the TSC's ratio to the crystal clock but not
// the clock, by family 6 model. Intel SDM 325462-093 Vol. 3B, 22.7.3, Table
// 22-95. Xeon W processors share model 0x55 with Xeon Scalable but run at
// 24MHz, so a clock is taken only where it agrees with the measurement.
static const struct {
    uint8_t model;
    uint32_t crystal_hz;
} crystal_clocks[] = {
    { 0x55, 25000000 },
    { 0x55, 24000000 },
    { 0x4e, 24000000 },
    { 0x5e, 24000000 },
    { 0x8e, 24000000 },
    { 0x9e, 24000000 },
    { 0x5c, 19200000 }
};

static uint64_t tsc_freq_nominal(uint64_t measured) {
    uint32_t eax, ebx, ecx, edx;
    if (!cpuid(0x15, 0, &eax, &ebx, &ecx, &edx) || eax == 0 || ebx == 0 || ecx != 0) {
        return 0;
    }
    uint32_t denominator = eax;
    uint32_t numerator = ebx;

    if (!cpuid(1, 0, &eax, &ebx, &ecx, &edx) || ((eax >> 8) & 0xf) != 6) {
        return 0;
    }
    uint8_t model = (uint8_t)(((eax >> 4) & 0xf) | ((eax >> 12) & 0xf0));

    for (size_t i = 0; i < SIZEOF_ARRAY(crystal_clocks); i++) {
        if (crystal_clocks[i].model != model) {
            continue;
        }
        uint64_t nominal = (uint64_t)crystal_clocks[i].crystal_hz * numerator / denominator;
        uint64_t error = nominal > measured ? nominal - measured : measured - nominal;
        if (error <= measured / 100) {
            return nominal;
        }
    }

    return 0;
}
#endif

void calibrate_tsc(void) {
    tsc_freq = tsc_freq_arch();
    if (tsc_freq != 0) {
        return;
    }

#if defined(UEFI)
    // Calibrate over 10ms. Run multiple rounds and take the smallest
    // (least SMI-disrupted) delta.
    #define EFI_CALIBRATION_STALL 10000
    #define EFI_CALIBRATION_ROUNDS 3

    uint64_t best_delta = 0;
    for (int round = 0; round < EFI_CALIBRATION_ROUNDS; round++) {
        uint64_t tsc_start = rdtsc();
        gBS->Stall(EFI_CALIBRATION_STALL);
        uint64_t tsc_end = rdtsc();

        if (tsc_end > tsc_start) {
            uint64_t delta = tsc_end - tsc_start;
            if (delta < best_delta || best_delta == 0) {
                best_delta = delta;
            }
        }
    }

    if (best_delta != 0) {
        tsc_freq = best_delta * (1000000ULL / EFI_CALIBRATION_STALL);
    }
#elif defined(BIOS)
    // Preferred over the PIT: it needs no programming, and it is still there
    // on a machine whose PIT is not.
    tsc_freq = calibrate_tsc_pm_timer();
    if (tsc_freq == 0) {
        tsc_freq = calibrate_tsc_pit();
    }
#endif

#if defined(__x86_64__) || defined(__i386__)
    uint64_t nominal = tsc_freq_nominal(tsc_freq);
    if (nominal != 0) {
        tsc_freq = nominal;
    }
#endif

    // A zero here is not a degraded boot: stall() becomes a no-op, so the SMP
    // bring-up delays vanish, and rdtsc_deadline() reads as no deadline at all.
    if (tsc_freq == 0) {
        panic(false, "Could not calibrate the TSC");
    }
}
