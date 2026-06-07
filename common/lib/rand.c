#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <lib/rand.h>
#include <sys/cpu.h>
#include <mm/pmm.h>

// TODO: Find where this mersenne twister implementation is inspired from
//       and properly credit the original author(s).

static bool rand_initialised = false;

#define n ((int)624)
#define m ((int)397)
#define matrix_a ((uint32_t)0x9908b0df)
#define msb ((uint32_t)0x80000000)
#define lsbs ((uint32_t)0x7fffffff)

static uint32_t *status;
static int ctr;

static uint32_t hw_entropy(void) {
#if defined (__x86_64__) || defined(__i386__)
    uint32_t eax, ebx, ecx, edx;

    if (cpuid(0x07, 0, &eax, &ebx, &ecx, &edx) && (ebx & (1 << 18))) {
        uint32_t val =
#if defined (__x86_64__)
            (uint32_t)rdseed(uint64_t); // Always do a 64-bit op on 64-bit to work around CPU bugs.
#elif defined (__i386__)
            rdseed(uint32_t);
#endif
        if (val != 0) return val;
    } else if (cpuid(0x01, 0, &eax, &ebx, &ecx, &edx) && (ecx & (1 << 30))) {
        uint32_t val =
#if defined (__x86_64__)
            (uint32_t)rdrand(uint64_t); // As above.
#elif defined (__i386__)
            rdrand(uint32_t);
#endif
        if (val != 0) return val;
    }
#elif defined (__aarch64__)
    // ARMv8.5-RNG: check ID_AA64ISAR0_EL1 RNDR field (bits [63:60])
    uint64_t isar0;
    asm volatile ("mrs %0, id_aa64isar0_el1" : "=r" (isar0));
    if ((isar0 >> 60) & 0xf) {
        uint64_t rndr;
        // RNDR register: s3_3_c2_c4_0
        bool ok;
        asm volatile (
            "mrs %0, s3_3_c2_c4_0\n\t"
            "cset %w1, ne"
            : "=r" (rndr), "=r" (ok)
            :
            : "cc"
        );
        if (ok) {
            return (uint32_t)rndr;
        }
    }
#endif

#if defined (UEFI)
    // Try EFI RNG protocol as a fallback for all UEFI platforms
    {
        EFI_GUID rng_guid = EFI_RNG_PROTOCOL_GUID;
        EFI_RNG_PROTOCOL *rng = NULL;
        if (gBS->LocateProtocol(&rng_guid, NULL, (void **)&rng) == EFI_SUCCESS && rng != NULL) {
            uint32_t val;
            if (rng->GetRNG(rng, NULL, sizeof(val), (UINT8 *)&val) == EFI_SUCCESS) {
                return val;
            }
        }
    }
#endif

    return 0;
}

static void init_rand(void) {
    uint32_t seed = ((uint32_t)0xc597060c * (uint32_t)rdtsc())
                  * ((uint32_t)0xce86d624)
                  ^ ((uint32_t)0xee0da130 * (uint32_t)rdtsc());

    uint32_t hw = hw_entropy();
    seed ^= hw;

    status = ext_mem_alloc_counted(n, sizeof(uint32_t));

    srand(seed);

    rand_initialised = true;
}

void srand(uint32_t s) {
    status[0] = s;
    for (ctr = 1; ctr < n; ctr++)
        status[ctr] = (1812433253 * (status[ctr - 1] ^ (status[ctr - 1] >> 30)) + ctr);
}

uint32_t rand32(void) {
    if (!rand_initialised)
        init_rand();

    const uint32_t mag01[2] = {0, matrix_a};

    if (ctr >= n) {
        for (int kk = 0; kk < n - m; kk++) {
            uint32_t y = (status[kk] & msb) | (status[kk + 1] & lsbs);
            status[kk] = status[kk + m] ^ (y >> 1) ^ mag01[y & 1];
        }

        for (int kk = n - m; kk < n - 1; kk++) {
            uint32_t y = (status[kk] & msb) | (status[kk + 1] & lsbs);
            status[kk] = status[kk + (m - n)] ^ (y >> 1) ^ mag01[y & 1];
        }

        uint32_t y = (status[n - 1] & msb) | (status[0] & lsbs);
        status[n - 1] = status[m - 1] ^ (y >> 1) ^ mag01[y & 1];

        ctr = 0;
    }

    uint32_t res = status[ctr++];

    res ^= (res >> 11);
    res ^= (res << 7) & 0x9d2c5680;
    res ^= (res << 15) & 0xefc60000;
    res ^= (res >> 18);

    return res;
}

uint64_t rand64(void) {
    return (((uint64_t)rand32()) << 32) | (uint64_t)rand32();
}
