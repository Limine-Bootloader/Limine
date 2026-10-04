#if defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <efi.h>
#include <lib/rng_seed.h>
#include <lib/misc.h>
#include <lib/libc.h>
#include <lib/print.h>

// Defined by Linux, not by the UEFI specification.
#define RNG_SEED_TABLE_GUID \
    { 0x1ce1e5bc, 0x7ceb, 0x42f2, { 0x81, 0xe5, 0x8a, 0xad, 0xf1, 0x80, 0xf5, 0x7b } }
#define RNG_SEED_VAR_NAME L"RandomSeed"

// Seed material past this many bytes goes unread.
#define RNG_SEED_MAX ((size_t)1024)

// 256 bits: the security level UEFI 2.11 section 37.5 requires of a DRBG
// behind the RNG protocol. Ask for twice that, so that a raw source short of
// full entropy still covers it.
#define RNG_SEED_MIN_REQUEST ((UINTN)32)
#define RNG_SEED_REQUEST ((UINTN)64)

// UEFI is little-endian on every port (UEFI 2.11 section 1.9.1), so a native
// store yields the little-endian count the table wants.
struct rng_seed_table {
    uint32_t size;
    uint8_t seed[];
};

static void seed_wipe(void *buf, size_t size) {
    memset(buf, 0, size);
    // The asm may read buf, so the stores above cannot be dropped as dead.
    asm volatile ("" :: "r" (buf) : "memory");
}

// Wrapping at the end of dst means every input byte reaches the part of the
// table that is read, rather than the overflow being cut off.
static void seed_fold(uint8_t *dst, size_t dst_len, size_t offset, const uint8_t *src, size_t len) {
    for (size_t i = 0; i < len; i++) {
        dst[(offset + i) % dst_len] ^= src[i];
    }
}

static size_t seed_request(EFI_RNG_PROTOCOL *rng, EFI_RNG_ALGORITHM *alg, uint8_t *buf) {
    UINTN len = RNG_SEED_REQUEST;
    EFI_STATUS status = rng->GetRNG(rng, alg, len, buf);

    // The source has too little banked for the full length; take the minimum.
    if (status == EFI_NOT_READY) {
        len = RNG_SEED_MIN_REQUEST;
        status = rng->GetRNG(rng, alg, len, buf);
    }

    if (status != EFI_SUCCESS) {
        printv("rng_seed: GetRNG (%s) failed (%X)\n",
                alg == NULL ? "default" : "raw", (uint64_t)status);
        return 0;
    }

    return len;
}

// CPU RNG instructions are deliberately not a source: Linux samples them
// itself under random.trust_cpu, and passing their output on here would
// have it credited under random.trust_bootloader instead.
static size_t seed_gather(uint8_t *buf) {
    EFI_GUID rng_guid = EFI_RNG_PROTOCOL_GUID;
    EFI_RNG_PROTOCOL *rng = NULL;

    EFI_STATUS status = gBS->LocateProtocol(&rng_guid, NULL, (void **)&rng);
    if (status != EFI_SUCCESS || rng == NULL) {
        printv("rng_seed: No EFI RNG protocol\n");
        return 0;
    }

    // Every driver has a default algorithm (UEFI 2.11 section 37.5.2); raw,
    // where offered, is the entropy source with no DRBG in front (section
    // 37.5.4). Taking both means neither path alone decides the seed.
    EFI_RNG_ALGORITHM raw = EFI_RNG_ALGORITHM_RAW;
    size_t filled = seed_request(rng, NULL, buf);
    filled += seed_request(rng, &raw, buf + filled);

    return filled;
}

// Material that a later boot could read again must never seed this one, so
// the variable is deleted before its bytes are used.
static size_t seed_take_variable(uint8_t *buf) {
    EFI_GUID guid = RNG_SEED_TABLE_GUID;
    UINTN size = RNG_SEED_MAX;

    EFI_STATUS status = gRT->GetVariable(RNG_SEED_VAR_NAME, &guid, NULL, &size, buf);
    if (status == EFI_NOT_FOUND) {
        return 0;
    }
    if (status != EFI_SUCCESS && status != EFI_BUFFER_TOO_SMALL) {
        printv("rng_seed: Failed to read RandomSeed (%X)\n", (uint64_t)status);
        return 0;
    }

    // GetVariable() cannot read part of a variable (UEFI 2.11 section 8.2.1),
    // so one longer than RNG_SEED_MAX can never be used. It is deleted all the
    // same, rather than left to take up variable storage.
    bool fits = status == EFI_SUCCESS && size <= RNG_SEED_MAX;

    // With no attributes and no data, this is a delete that need not match
    // the attributes the variable was set with (UEFI 2.11 section 8.2.3).
    status = gRT->SetVariable(RNG_SEED_VAR_NAME, &guid, 0, 0, NULL);
    if (status != EFI_SUCCESS) {
        seed_wipe(buf, RNG_SEED_MAX);
        printv("rng_seed: Failed to delete RandomSeed (%X), not using it\n", (uint64_t)status);
        return 0;
    }

    if (!fits) {
        printv("rng_seed: Deleted a %U-byte RandomSeed unread, over the %u-byte limit\n",
                (uint64_t)size, (uint32_t)RNG_SEED_MAX);
        return 0;
    }

    printv("rng_seed: Took %u bytes from RandomSeed\n", (uint32_t)size);
    return size;
}

void rng_seed_install(void) {
    EFI_GUID table_guid = RNG_SEED_TABLE_GUID;

    // Nothing says an earlier stage's table is aligned, so it is only ever
    // accessed as bytes.
    uint8_t *prev = NULL;
    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *entry = &gST->ConfigurationTable[i];
        if (memcmp(&entry->VendorGuid, &table_guid, sizeof(EFI_GUID)) == 0) {
            prev = entry->VendorTable;
            break;
        }
    }

    // The count is all an earlier stage tells us about its allocation.
    // Trusting it beyond RNG_SEED_MAX risks wiping memory that is not the
    // table's.
    size_t prev_size = 0;
    if (prev != NULL) {
        uint32_t prev_count;
        memcpy(&prev_count, prev, sizeof(prev_count));
        prev_size = MIN((size_t)prev_count, RNG_SEED_MAX);
    }

    uint8_t fresh[2 * RNG_SEED_REQUEST + RNG_SEED_MAX];
    size_t fresh_size = seed_gather(fresh);
    fresh_size += seed_take_variable(fresh + fresh_size);

    if (fresh_size == 0) {
        seed_wipe(fresh, sizeof(fresh));
        if (prev != NULL) {
            printv("rng_seed: No new entropy, keeping the existing table\n");
        } else {
            printv("rng_seed: No entropy available, not installing a table\n");
        }
        return;
    }

    size_t size = MIN(prev_size + fresh_size, RNG_SEED_MAX);
    size_t table_size = sizeof(struct rng_seed_table) + size;

    // Read after ExitBootServices(), so not EfiLoaderData, which the OS may
    // reuse at once. EfiACPIReclaimMemory is kept until ACPI is enabled (UEFI
    // 2.11 section 7.2, table 7.6), which is long enough, whereas
    // EfiRuntimeServicesData would pin the seed for the life of the system.
    struct rng_seed_table *table = NULL;
    EFI_STATUS status = gBS->AllocatePool(EfiACPIReclaimMemory, table_size, (void **)&table);
    if (status != EFI_SUCCESS) {
        seed_wipe(fresh, sizeof(fresh));
        printv("rng_seed: Failed to allocate the table (%X)\n", (uint64_t)status);
        return;
    }

    memset(table, 0, table_size);
    if (prev != NULL) {
        seed_fold(table->seed, size, 0, prev + offsetof(struct rng_seed_table, seed), prev_size);
    }
    seed_fold(table->seed, size, prev_size, fresh, fresh_size);
    table->size = (uint32_t)size;

    seed_wipe(fresh, sizeof(fresh));

    status = gBS->InstallConfigurationTable(&table_guid, table);
    if (status != EFI_SUCCESS) {
        seed_wipe(table, table_size);
        gBS->FreePool(table);
        printv("rng_seed: Failed to install the table (%X)\n", (uint64_t)status);
        return;
    }

    // FreePool() only takes AllocatePool() memory (UEFI 2.11 section 7.2.5),
    // and nothing says that is what an earlier stage used, so its table is
    // left allocated, but with nothing secret in it.
    if (prev != NULL) {
        seed_wipe(prev, offsetof(struct rng_seed_table, seed) + prev_size);
    }

    printv("rng_seed: Installed a %u-byte seed table at %p (%u bytes carried over)\n",
            (uint32_t)size, table, (uint32_t)prev_size);
}

#endif
