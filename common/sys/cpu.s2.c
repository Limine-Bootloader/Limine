#include <stdint.h>
#include <stdbool.h>
#include <sys/cpu.h>
#if defined(UEFI)
#include <efi.h>
#include <lib/misc.h>
#endif

uint64_t tsc_freq = 0;

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
    // Calibrate TSC using PIT channel 2.
    // PIT oscillator frequency: 1193182 Hz
    // Count of 11932 gives ~10ms calibration interval.
    // Run multiple rounds and take the highest (least SMI-disrupted) result.
    #define PIT_CALIBRATION_COUNT 11932
    #define PIT_CALIBRATION_ROUNDS 3

    uint8_t port61 = inb(0x61);

    uint64_t best_delta = 0;
    for (int round = 0; round < PIT_CALIBRATION_ROUNDS; round++) {
        outb(0x61, port61 & ~0x03); // disable gate and speaker
        outb(0x43, 0xb0); // channel 2, lobyte/hibyte, mode 0, binary
        outb(0x42, PIT_CALIBRATION_COUNT & 0xff);
        outb(0x42, (PIT_CALIBRATION_COUNT >> 8) & 0xff);

        outb(0x61, (inb(0x61) | 0x01)); // enable gate to start counting
        uint64_t tsc_start = rdtsc();

        while ((inb(0x61) & 0x20) == 0); // wait for output high
        uint64_t tsc_end = rdtsc();

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
}
