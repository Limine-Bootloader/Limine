#if defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <efi.h>
#include <lib/rng_seed.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <lib/libc.h>

#define LINUX_EFI_RANDOM_SEED_TABLE_GUID \
    { 0x1ce1e5bc, 0x7ceb, 0x42f2, { 0x81, 0xe5, 0x8a, 0xad, 0xf1, 0x80, 0xf5, 0x7b } }

#define EFI_RANDOM_SEED_SIZE 32

struct linux_efi_random_seed {
    uint32_t size;
    uint8_t  bits[];
} __attribute__((packed));

// Pull entropy from EFI_RNG_PROTOCOL while boot services are alive, mix in
// the NVRAM-resident "RandomSeed" variable if the OS left one for us, and
// publish the result as the LINUX_EFI_RANDOM_SEED_TABLE configuration table
// for the kernel's RNG to consume during early boot. Limine bypasses the
// EFI stub that would normally do this.
void rng_seed_install(void) {
    EFI_GUID rng_table_guid = LINUX_EFI_RANDOM_SEED_TABLE_GUID;

    // The RNG protocol is optional; the NVRAM seed alone is also a valid
    // source.
    EFI_GUID rng_guid = EFI_RNG_PROTOCOL_GUID;
    EFI_RNG_PROTOCOL *rng = NULL;
    if (gBS->LocateProtocol(&rng_guid, NULL, (void **)&rng) != EFI_SUCCESS) {
        rng = NULL;
    }

    // Probe the NVRAM "RandomSeed" variable; if the OS left one for us,
    // we'll consume and delete it so it isn't reused on the next boot.
    UINTN nv_seed_size = 0;
    EFI_STATUS probe = gRT->GetVariable(L"RandomSeed", &rng_table_guid,
                                        NULL, &nv_seed_size, NULL);
    if (probe != EFI_BUFFER_TOO_SMALL || nv_seed_size > 512) {
        nv_seed_size = 0;
    }

    // A prior boot stage (shim, another stub) may have installed a seed
    // already. Preserve it by concatenating rather than overwriting.
    struct linux_efi_random_seed *prev_seed = NULL;
    uint32_t prev_seed_size = 0;
    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
        if (memcmp(&gST->ConfigurationTable[i].VendorGuid,
                   &rng_table_guid, sizeof(EFI_GUID)) == 0) {
            prev_seed = gST->ConfigurationTable[i].VendorTable;
            if (prev_seed->size <= 512) {
                prev_seed_size = prev_seed->size;
            }
            break;
        }
    }

    UINTN rng_bytes = (rng != NULL) ? EFI_RANDOM_SEED_SIZE : 0;

    if (rng_bytes == 0 && nv_seed_size == 0 && prev_seed_size == 0) {
        return;
    }

    UINTN total_size = sizeof(struct linux_efi_random_seed)
                     + rng_bytes + nv_seed_size + prev_seed_size;

    struct linux_efi_random_seed *seed = NULL;
    EFI_STATUS status = gBS->AllocatePool(EfiACPIReclaimMemory, total_size,
                                          (void **)&seed);
    if (status != EFI_SUCCESS) {
        printv("rng: failed to allocate random seed table: %X\n", (uint64_t)status);
        return;
    }

    memset(seed, 0, total_size);

    UINTN offset = 0;

    // EFI_RNG_PROTOCOL output. Prefer the raw algorithm.
    if (rng != NULL) {
        EFI_GUID rng_algo_raw = EFI_RNG_ALGORITHM_RAW;
        status = rng->GetRNG(rng, &rng_algo_raw, EFI_RANDOM_SEED_SIZE, seed->bits);
        if (status == EFI_UNSUPPORTED) {
            status = rng->GetRNG(rng, NULL, EFI_RANDOM_SEED_SIZE, seed->bits);
        }
        if (status == EFI_SUCCESS) {
            offset += EFI_RANDOM_SEED_SIZE;
        } else {
            printv("rng: GetRNG failed: %X\n", (uint64_t)status);
        }
    }

    // NVRAM "RandomSeed" variable, then delete it so the same bytes
    // aren't reused on the next boot.
    if (nv_seed_size > 0) {
        UINTN got_size = nv_seed_size;
        status = gRT->GetVariable(L"RandomSeed", &rng_table_guid, NULL,
                                  &got_size, seed->bits + offset);
        if (status == EFI_SUCCESS) {
            gRT->SetVariable(L"RandomSeed", &rng_table_guid, 0, 0, NULL);
            offset += got_size;
        } else {
            // Read failed despite probe succeeding. Wipe the slot to avoid
            // publishing stale heap contents.
            volatile uint8_t *p = (volatile uint8_t *)(seed->bits + offset);
            for (size_t i = 0; i < nv_seed_size; i++) {
                p[i] = 0;
            }
            asm volatile ("" ::: "memory");
        }
    }

    // Previous-stage seed.
    if (prev_seed_size > 0) {
        memcpy(seed->bits + offset, prev_seed->bits, prev_seed_size);
        offset += prev_seed_size;
    }

    if (offset == 0) {
        volatile uint8_t *p = (volatile uint8_t *)seed;
        for (size_t i = 0; i < total_size; i++) {
            p[i] = 0;
        }
        asm volatile ("" ::: "memory");
        gBS->FreePool(seed);
        return;
    }

    seed->size = (uint32_t)offset;

    status = gBS->InstallConfigurationTable(&rng_table_guid, seed);
    if (status != EFI_SUCCESS) {
        printv("rng: failed to install random seed table: %X\n", (uint64_t)status);
        volatile uint8_t *p = (volatile uint8_t *)seed;
        for (size_t i = 0; i < total_size; i++) {
            p[i] = 0;
        }
        asm volatile ("" ::: "memory");
        gBS->FreePool(seed);
        return;
    }

    if (prev_seed_size > 0) {
        volatile uint8_t *p = (volatile uint8_t *)prev_seed;
        size_t prev_total = sizeof(struct linux_efi_random_seed) + prev_seed_size;
        for (size_t i = 0; i < prev_total; i++) {
            p[i] = 0;
        }
        asm volatile ("" ::: "memory");
        // Assumes the prior publisher used AllocatePool.
        gBS->FreePool(prev_seed);
    }

    printv("rng: installed %u-byte random seed as configuration table\n",
           seed->size);
}

#endif
