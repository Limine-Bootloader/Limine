#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <lib/libc.h>
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

size_t hw_entropy(void *buf, size_t size) {
    uint8_t *out = buf;
    size_t filled = 0;

#if defined (__x86_64__) || defined(__i386__)
    uint32_t eax, ebx, ecx, edx;
    bool have_rdseed = cpuid(0x07, 0, &eax, &ebx, &ecx, &edx) && (ebx & (1 << 18));
    bool have_rdrand = cpuid(0x01, 0, &eax, &ebx, &ecx, &edx) && (ecx & (1 << 30));

    while (filled < size && (have_rdseed || have_rdrand)) {
        uint32_t val;
        if (have_rdseed) {
#if defined (__x86_64__)
            val = (uint32_t)rdseed(uint64_t); // Always do a 64-bit op on 64-bit to work around CPU bugs.
#elif defined (__i386__)
            val = rdseed(uint32_t);
#endif
        } else {
#if defined (__x86_64__)
            val = (uint32_t)rdrand(uint64_t); // As above.
#elif defined (__i386__)
            val = rdrand(uint32_t);
#endif
        }

        // A zero result means the instruction never set carry across all of its
        // retries; treat the source as exhausted rather than spinning forever.
        if (val == 0) {
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

static void init_rand(void) {
    uint32_t seed = ((uint32_t)0xc597060c * (uint32_t)rdtsc())
                  * ((uint32_t)0xce86d624)
                  ^ ((uint32_t)0xee0da130 * (uint32_t)rdtsc());

    uint32_t hw = 0;
    hw_entropy(&hw, sizeof(hw));
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
