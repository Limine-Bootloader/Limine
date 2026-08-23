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

static uint64_t calibrate_tsc_pm_timer(void) {
    uint16_t port;
    uint32_t mask;
    if (!pm_timer_find(&port, &mask)) {
        return 0;
    }

    uint64_t best_delta = 0;
    uint32_t best_elapsed = 0;

    for (int round = 0; round < PM_TIMER_CALIBRATION_ROUNDS; round++) {
        uint32_t start = ind(port) & mask;
        uint64_t tsc_start = rdtsc();

        uint32_t elapsed = 0;
        uint32_t seen = 0;
        size_t stalled = 0;
        for (size_t polls = 0; polls < PM_TIMER_MAX_POLLS; polls++) {
            elapsed = ((ind(port) & mask) - start) & mask;
            if (elapsed >= PM_TIMER_CALIBRATION_TICKS) {
                break;
            }
            if (elapsed != seen) {
                seen = elapsed;
                stalled = 0;
            } else if (++stalled >= PM_TIMER_MAX_STALLED_POLLS) {
                break;
            }
        }
        uint64_t tsc_end = rdtsc();

        if (elapsed < PM_TIMER_CALIBRATION_TICKS) {
            return 0;
        }

        if (tsc_end > tsc_start) {
            uint64_t delta = tsc_end - tsc_start;
            if (delta < best_delta || best_delta == 0) {
                best_delta = delta;
                best_elapsed = elapsed;
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
    // Preferred over the PIT below: it needs no programming, and it is still
    // there on a machine whose PIT is not.
    tsc_freq = calibrate_tsc_pm_timer();
    if (tsc_freq != 0) {
        return;
    }

    // Calibrate TSC using PIT channel 2.
    // PIT oscillator frequency: 1193182 Hz
    // Count of 11932 gives ~10ms calibration interval.
    // Run multiple rounds and take the smallest (least SMI-disrupted) result.
    #define PIT_CALIBRATION_COUNT 11932
    #define PIT_CALIBRATION_ROUNDS 3

    // A gate that never asserts leaves OUT low, so the wait never ends. The
    // count is sized for the fastest port access, which is what decides how
    // many reads 10ms takes; on a slow one it is what the give-up costs.
    #define PIT_MAX_POLLS 1000000

    uint8_t port61 = inb(0x61);

    uint64_t best_delta = 0;
    for (int round = 0; round < PIT_CALIBRATION_ROUNDS; round++) {
        outb(0x61, port61 & ~0x03); // disable gate and speaker
        outb(0x43, 0xb0); // channel 2, lobyte/hibyte, mode 0, binary
        outb(0x42, PIT_CALIBRATION_COUNT & 0xff);
        outb(0x42, (PIT_CALIBRATION_COUNT >> 8) & 0xff);

        // Mode 0 drives OUT low on the control word and raises it only at
        // terminal count, so a port already high is not a counting timer.
        if ((inb(0x61) & 0x20) != 0) {
            continue;
        }

        outb(0x61, (inb(0x61) | 0x01)); // enable gate to start counting
        uint64_t tsc_start = rdtsc();

        bool counted = false;
        for (size_t polls = 0; polls < PIT_MAX_POLLS; polls++) {
            if ((inb(0x61) & 0x20) != 0) {
                counted = true;
                break;
            }
        }
        uint64_t tsc_end = rdtsc();

        if (!counted) {
            continue;
        }

        if (tsc_end > tsc_start) {
            uint64_t delta = tsc_end - tsc_start;
            if (delta < best_delta || best_delta == 0) {
                best_delta = delta;
            }
        }
    }

    outb(0x61, port61); // restore

    if (best_delta != 0) {
        tsc_freq = best_delta * 1193182 / PIT_CALIBRATION_COUNT;
    }
#endif

    // A zero here is not a degraded boot: stall() becomes a no-op, so the SMP
    // bring-up delays vanish, and rdtsc_deadline() reads as no deadline at all.
    if (tsc_freq == 0) {
        panic(false, "Could not calibrate the TSC");
    }
}
