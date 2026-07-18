#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <lib/libc.h>
#include <lib/rand.h>
#include <sys/cpu.h>

// PCG32 (PCG-XSH-RR 64/32, single-stream variant) of M. E. O'Neill 2014.
// For security-sensitive randomness use safe_rand32()/safe_rand64() instead.

#define PCG_MULTIPLIER ((uint64_t)6364136223846793005)
#define PCG_INCREMENT  ((uint64_t)1442695040888963407) // must be odd

static bool rand_initialised = false;
static uint64_t pcg_state;

size_t hw_entropy(void *buf, size_t size) {
    uint8_t *out = buf;
    size_t filled = 0;

#if defined (__x86_64__) || defined(__i386__)
    uint32_t eax, ebx, ecx, edx;
    bool have_rdseed = cpuid(0x07, 0, &eax, &ebx, &ecx, &edx) && (ebx & (1 << 18));
    bool have_rdrand = cpuid(0x01, 0, &eax, &ebx, &ecx, &edx) && (ecx & (1 << 30));

    while (filled < size && (have_rdseed || have_rdrand)) {
        uint32_t val;
        bool ok;
        if (have_rdseed) {
#if defined (__x86_64__)
            uint64_t wide;
            ok = rdseed(uint64_t, &wide); // Always do a 64-bit op on 64-bit to work around CPU bugs.
            val = (uint32_t)wide;
#elif defined (__i386__)
            ok = rdseed(uint32_t, &val);
#endif
        } else {
#if defined (__x86_64__)
            uint64_t wide;
            ok = rdrand(uint64_t, &wide); // As above.
            val = (uint32_t)wide;
#elif defined (__i386__)
            ok = rdrand(uint32_t, &val);
#endif
        }

        // Carry stays clear only when every retry failed, i.e. the source is
        // exhausted; a genuine zero draw sets carry and must be kept.
        if (!ok) {
            break;
        }

        size_t chunk = size - filled < sizeof(val) ? size - filled : sizeof(val);
        memcpy(out + filled, &val, chunk);
        filled += chunk;
    }
#elif defined (__aarch64__)
    // ARMv8.5-RNG: check ID_AA64ISAR0_EL1 RNDR field (bits [63:60])
    uint64_t isar0;
    asm volatile ("mrs %0, id_aa64isar0_el1" : "=r" (isar0));
    if ((isar0 >> 60) & 0xf) {
        while (filled < size) {
            uint64_t rndr;
            bool ok;
            // RNDR register: s3_3_c2_c4_0
            asm volatile (
                "mrs %0, s3_3_c2_c4_0\n\t"
                "cset %w1, ne"
                : "=r" (rndr), "=r" (ok)
                :
                : "cc"
            );
            if (!ok) {
                break;
            }

            size_t chunk = size - filled < sizeof(rndr) ? size - filled : sizeof(rndr);
            memcpy(out + filled, &rndr, chunk);
            filled += chunk;
        }
    }
#endif

#if defined (UEFI)
    // Try the EFI RNG protocol as a fallback for any bytes still missing.
    if (filled < size) {
        EFI_GUID rng_guid = EFI_RNG_PROTOCOL_GUID;
        EFI_RNG_PROTOCOL *rng = NULL;
        if (gBS->LocateProtocol(&rng_guid, NULL, (void **)&rng) == EFI_SUCCESS && rng != NULL) {
            if (rng->GetRNG(rng, NULL, size - filled, out + filled) == EFI_SUCCESS) {
                filled = size;
            }
        }
    }
#endif

    return filled;
}

static uint32_t pcg_next(void) {
    uint64_t old = pcg_state;
    pcg_state = old * PCG_MULTIPLIER + PCG_INCREMENT;
    uint32_t xorshifted = (uint32_t)(((old >> 18) ^ old) >> 27);
    uint32_t rot = (uint32_t)(old >> 59);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

void srand(uint32_t s) {
    // Canonical PCG seeding: advance, fold the seed in, advance again.
    pcg_state = 0;
    pcg_next();
    pcg_state += s;
    pcg_next();
    rand_initialised = true;
}

static void init_rand(void) {
    uint64_t seed = 0;
    hw_entropy(&seed, sizeof(seed));
    seed ^= (uint64_t)0xc597060cee0da130 * rdtsc();
    seed ^= (uint64_t)0xce86d6249d2c5680 * rdtsc();
    pcg_state = 0;
    pcg_next();
    pcg_state += seed;
    pcg_next();
    rand_initialised = true;
}

uint32_t rand32(void) {
    if (!rand_initialised)
        init_rand();

    return pcg_next();
}

uint64_t rand64(void) {
    return (((uint64_t)rand32()) << 32) | (uint64_t)rand32();
}

// Hardware-entropy-backed variants for security-sensitive consumers such as
// ASLR. The PCG stream is deterministic given its seed and thus predictable if
// any outputs leak; these return raw hardware entropy when available and fall
// back to the PRNG only when no hardware source could provide the full width.
uint32_t safe_rand32(void) {
    uint32_t v;
    if (hw_entropy(&v, sizeof(v)) == sizeof(v)) {
        return v;
    }
    return rand32();
}

uint64_t safe_rand64(void) {
    uint64_t v;
    if (hw_entropy(&v, sizeof(v)) == sizeof(v)) {
        return v;
    }
    return rand64();
}
