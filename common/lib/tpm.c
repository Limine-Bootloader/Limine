#if defined(UEFI)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <efi.h>
#include <lib/tpm.h>
#include <lib/getchar.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <lib/term.h>
#include <lib/tpm_mem.h>
#include <mm/pmm.h>

// Event log records, as laid out by the TCG EFI Protocol Specification, Family
// "2.0", Level 00 Revision 00.13. They are densely packed, and little-endian
// like every Limine target (section 3.1).

// Real logs are far smaller; a larger one is taken to be corrupt.
#define TCG_LOG_MAX_SIZE 0x400000

// Room kept past the copy of a crypto-agile log for the events logged after
// its capture, such as ExitBootServices()'s, which are a few small records.
#define TCG_LOG_HEADROOM 0x2000

// TPMs have a handful of PCR banks; the cap keeps record parsing cheap.
#define TCG_LOG_MAX_ALGORITHMS 16

// TCG EFI Protocol section 7, UEFI 2.11 section 38.3.
#define TCG_FINAL_EVENTS_TABLE_VERSION 1

// Section 5.1, without the trailing event data.
struct tcg_pcr_event {
    uint32_t pcr_index;
    uint32_t event_type;
    uint8_t digest[20];
    uint32_t event_size;
} __attribute__((packed));

// Section 5.2, up to the TPMT_HA list of TCG_PCR_EVENT2.Digests.
struct tcg_pcr_event2_head {
    uint32_t pcr_index;
    uint32_t event_type;
    uint32_t digest_count;
} __attribute__((packed));

// Section 5.3, the log header's event data up to its digestSizes array.
struct tcg_efi_spec_id_event {
    uint8_t signature[16];
    uint32_t platform_class;
    uint8_t spec_version_minor;
    uint8_t spec_version_major;
    uint8_t spec_errata;
    uint8_t uintn_size;
    uint32_t number_of_algorithms;
} __attribute__((packed));

struct tcg_efi_spec_id_event_algorithm_size {
    uint16_t algorithm_id;
    uint16_t digest_size;
} __attribute__((packed));

// The digestSizes of a crypto-agile log's header, needed to walk its records.
struct tcg_digest_sizes {
    uint32_t count;
    struct tcg_efi_spec_id_event_algorithm_size algorithms[TCG_LOG_MAX_ALGORITHMS];
};

// The native handoff exports one log. Under Secure Boot, the TPM and CC
// registers are independent and each available interface needs an extend.
static struct efi_tcg2_protocol *tcg2 = NULL;
static struct efi_cc_measurement_protocol *cc = NULL;
static uint32_t required_pcr_banks;
static bool tpm_log_truncated;
static size_t tpm_check_log(
    size_t previous_size,
    const struct efi_tcg2_event *expected,
    bool after_load_image
);

static bool secure_boot_capable(const struct efi_tcg2_boot_service_capability *cap) {
    size_t min_size = offsetof(struct efi_tcg2_boot_service_capability, active_pcr_banks)
                    + sizeof(efi_tcg2_event_algorithm_bitmap);
    return cap->size >= min_size
        && (cap->structure_version.major > 1
         || (cap->structure_version.major == 1 && cap->structure_version.minor >= 1))
        && (cap->protocol_version.major > 1
         || (cap->protocol_version.major == 1 && cap->protocol_version.minor >= 1))
        && (cap->active_pcr_banks & EFI_TCG2_BOOT_HASH_ALG_SHA256) != 0
        && (cap->supported_event_logs & EFI_TCG2_EVENT_LOG_FORMAT_TCG_2) != 0;
}

void tpm_init(void) {
    struct efi_guid tcg2_guid = EFI_TCG2_PROTOCOL_GUID;
    struct efi_tcg2_protocol *tcg2_proto = NULL;
    efi_status status = gBS->locate_protocol(&tcg2_guid, NULL, (void **)&tcg2_proto);
    if (status == EFI_SUCCESS && tcg2_proto != NULL && tcg2_proto->get_capability != NULL
     && tcg2_proto->get_event_log != NULL && tcg2_proto->hash_log_extend_event != NULL) {
        struct efi_tcg2_boot_service_capability cap;
        memset(&cap, 0, sizeof(cap));
        cap.size = sizeof(cap);
        status = tcg2_proto->get_capability(tcg2_proto, &cap);
        if (status == EFI_SUCCESS
         && cap.size > offsetof(struct efi_tcg2_boot_service_capability, tpm_present_flag)
         && cap.tpm_present_flag != 0 && (!firmware_secure_boot || secure_boot_capable(&cap))) {
            tcg2 = tcg2_proto;
            printv("tpm: TCG2 protocol located, TPM present\n");
            if (!firmware_secure_boot) {
                return;
            }
            const uint32_t known_banks =
                EFI_TCG2_BOOT_HASH_ALG_SHA1 | EFI_TCG2_BOOT_HASH_ALG_SHA256
              | EFI_TCG2_BOOT_HASH_ALG_SHA384 | EFI_TCG2_BOOT_HASH_ALG_SHA512
              | EFI_TCG2_BOOT_HASH_ALG_SM3_256 | EFI_TCG2_BOOT_HASH_ALG_SHA3_256
              | EFI_TCG2_BOOT_HASH_ALG_SHA3_384 | EFI_TCG2_BOOT_HASH_ALG_SHA3_512;
            required_pcr_banks = cap.active_pcr_banks & known_banks;
            tpm_validate_log(false);
        }
    }

    struct efi_guid cc_guid = EFI_CC_MEASUREMENT_PROTOCOL_GUID;
    struct efi_cc_measurement_protocol *cc_proto = NULL;
    status = gBS->locate_protocol(&cc_guid, NULL, (void **)&cc_proto);
    if (status == EFI_SUCCESS && cc_proto != NULL && cc_proto->get_capability != NULL
     && cc_proto->get_event_log != NULL && cc_proto->hash_log_extend_event != NULL
     && cc_proto->map_pcr_to_mr_index != NULL) {
        struct efi_cc_boot_service_capability cap;
        memset(&cap, 0, sizeof(cap));
        cap.size = sizeof(cap);
        status = cc_proto->get_capability(cc_proto, &cap);
        size_t min_size = offsetof(struct efi_cc_boot_service_capability, cc_type)
                        + sizeof(struct efi_cc_type);
        if (status == EFI_SUCCESS && cap.size >= min_size && (!firmware_secure_boot
         || (cap.supported_event_logs & EFI_CC_EVENT_LOG_FORMAT_TCG_2) != 0)) {
            cc = cc_proto;
            const char *cc_name = "unknown";
            switch (cap.cc_type.type) {
                case EFI_CC_TYPE_AMD_SEV:   cc_name = "AMD SEV";   break;
                case EFI_CC_TYPE_INTEL_TDX: cc_name = "Intel TDX"; break;
            }
            printv("tpm: CC measurement protocol located (type: %s)\n", cc_name);
        }
    }
    if (firmware_secure_boot) {
        measured_boot = tpm_present();
    }
}

bool tpm_present(void) {
    return tcg2 != NULL || cc != NULL;
}

static bool tpm_secure_boot_available(void) {
    return required_pcr_banks != 0;
}

// EFI_VOLUME_FULL has the PCR extended and only the event left out of the log
// (TCG EFI Protocol Specification Level 00 Revision 00.13, Table 12; UEFI 2.11
// section 38.2.4), so the component stands measured.
static void tpm_log_full(void) {
    static bool warned = false;

    if (!warned) {
        warned = true;
        // The graphics terminal has already been released at LoadImage.
        if (term_backend == _NOT_READY && !efi_boot_services_exited) {
            term_fallback();
        }
        quiet = false;
        print("WARNING: tpm: The event log is full; PCR replay may be incomplete.\n");
    }
}

static void tpm_extend_failed(void) {
    print("         Press Y to continue, press any other key to panic...");

    char ch = getchar();
    print("\n");
    if (ch != 'Y' && ch != 'y') {
        panic(false, "tpm: refusing to boot with an unmeasured component");
    }
}

static void tpm_measure_event(uint32_t pcr, uint32_t event_type,
    const void *data, size_t data_size, const void *desc_prefix, size_t prefix_len,
    const void *desc_value, size_t value_len, size_t terminator_size) {
    bool sb_tcg2 = tpm_secure_boot_available();
    bool sb_cc = firmware_secure_boot && cc != NULL;
    if ((sb_tcg2 || sb_cc)
     && (!measured_boot || (tcg2 == NULL && cc == NULL) || data == NULL)) {
        panic(false, "tpm: authenticated boot cannot skip a measurement");
    }
    if (!measured_boot || data == NULL) {
        return;
    }

    size_t desc_len = CHECKED_ADD(prefix_len, value_len,
        panic(false, "tpm: event description size overflow"));
    desc_len = CHECKED_ADD(desc_len, terminator_size,
        panic(false, "tpm: event description size overflow"));

    if (tcg2 != NULL) {
        size_t event_size = CHECKED_ADD(offsetof(struct efi_tcg2_event, event), desc_len,
            panic(false, "tpm: event size overflow"));
        if (event_size > UINT32_MAX) {
            panic(false, "tpm: event is too large");
        }

        struct efi_tcg2_event *event = ext_mem_alloc(event_size);
        event->size = (uint32_t)event_size;
        event->header.header_size = sizeof(struct efi_tcg2_event_header);
        event->header.header_version = 1;
        event->header.pcr_index = pcr;
        event->header.event_type = event_type;
        if (prefix_len > 0) {
            memcpy(event->event, desc_prefix, prefix_len);
        }
        if (value_len > 0) {
            memcpy(event->event + prefix_len, desc_value, value_len);
        }

        size_t previous_size = sb_tcg2 ? tpm_check_log(0, NULL, false) : 0;
        efi_status status = tcg2->hash_log_extend_event(
            tcg2, 0,
            (efi_physical_address)(uintptr_t)data, (uint64_t)data_size,
            event);
        if (sb_tcg2) {
            if (status == EFI_VOLUME_FULL) {
                tpm_log_truncated = true;
                tpm_log_full();
            } else if (status != EFI_SUCCESS) {
                pmm_free(event, event_size);
                panic(false, "tpm: authenticated boot measurement failed (%X)",
                      (uint64_t)status);
            } else {
                tpm_check_log(previous_size, event, false);
            }
        } else if (status == EFI_VOLUME_FULL) {
            tpm_log_full();
        } else if (status != EFI_SUCCESS) {
            quiet = false;
            print("WARNING: tpm: HashLogExtendEvent for PCR %u failed: %X\n"
                  "         This component has not been measured.\n",
                  pcr, (uint64_t)status);
            tpm_extend_failed();
        }

        pmm_free(event, event_size);
    }
    if (cc != NULL) {
        // CC platforms expose Memory Reference (MR) registers rather than
        // PCRs. The protocol provides a translation from a requested PCR
        // index to the platform's corresponding MR index.
        efi_cc_mr_index mr_index;
        efi_status status = cc->map_pcr_to_mr_index(cc, pcr, &mr_index);
        if (status != EFI_SUCCESS) {
            if (sb_cc) {
                panic(false, "tpm: authenticated boot measurement register unavailable (%X)",
                      (uint64_t)status);
            }
            quiet = false;
            print("WARNING: tpm: no measurement register for PCR %u: %X\n"
                  "         This component has not been measured.\n",
                  pcr, (uint64_t)status);
            tpm_extend_failed();
            return;
        }

        size_t event_size = CHECKED_ADD(offsetof(struct efi_cc_event, event), desc_len,
            panic(false, "tpm: event size overflow"));
        if (event_size > UINT32_MAX) {
            panic(false, "tpm: event is too large");
        }

        struct efi_cc_event *event = ext_mem_alloc(event_size);
        event->size = (uint32_t)event_size;
        event->header.header_size = sizeof(struct efi_cc_event_header);
        event->header.header_version = EFI_CC_EVENT_HEADER_VERSION;
        event->header.mr_index = mr_index;
        event->header.event_type = event_type;
        if (prefix_len > 0) {
            memcpy(event->event, desc_prefix, prefix_len);
        }
        if (value_len > 0) {
            memcpy(event->event + prefix_len, desc_value, value_len);
        }

        status = cc->hash_log_extend_event(
            cc, 0,
            (efi_physical_address)(uintptr_t)data, (uint64_t)data_size,
            event);
        if (status == EFI_VOLUME_FULL) {
            tpm_log_full();
        } else if (status != EFI_SUCCESS) {
            if (sb_cc) {
                pmm_free(event, event_size);
                panic(false, "tpm: authenticated boot measurement failed (%X)",
                      (uint64_t)status);
            }
            quiet = false;
            print("WARNING: tpm: CC HashLogExtendEvent for PCR %u (MR %u) failed: %X\n"
                  "         This component has not been measured.\n",
                  pcr, (uint32_t)mr_index, (uint64_t)status);
            tpm_extend_failed();
        }

        pmm_free(event, event_size);
    }
}

void tpm_measure(uint32_t pcr, uint32_t event_type,
    const void *data, size_t data_size, const char *desc_prefix, const char *desc_value) {
    if (!measured_boot || data == NULL) {
        return;
    }
    tpm_measure_event(pcr, event_type, data, data_size,
        desc_prefix, desc_prefix != NULL ? strlen(desc_prefix) : 0,
        desc_value, desc_value != NULL ? strlen(desc_value) : 0, 1);
}

void tpm_measure_config(const void *data, size_t size) {
    if (!firmware_secure_boot) {
        tpm_measure(TPM_PCR_LOADED_IMAGES, TPM_EV_IPL, data, size, "limine_cfg", NULL);
        return;
    }
    if (!tpm_present()) {
        return;
    }
    struct {
        uint32_t id;
        uint32_t size;
        char description[sizeof("limine.conf")];
    } __attribute__((packed)) tag = {
        .id = TPM_TAG_LIMINE_CONFIG,
        .size = sizeof(tag.description),
        .description = "limine.conf"
    };
    tpm_measure_event(TPM_PCR_LOADER_CONFIG, TPM_EV_EVENT_TAG, data, size,
        &tag, sizeof(tag), NULL, 0, 0);
}

void tpm_measure_load_options(const uint16_t *options, size_t bytes) {
    if (!firmware_secure_boot || !tpm_present()) {
        return;
    }
    if (options == NULL || bytes < sizeof(uint16_t) || bytes % sizeof(uint16_t) != 0
     || options[bytes / sizeof(uint16_t) - 1] != 0) {
        panic(false, "tpm: invalid UTF-16 load options");
    }
    tpm_measure_event(TPM_PCR_KERNEL_CONFIG, TPM_EV_IPL,
        options, bytes, NULL, 0, options, bytes, 0);
}

void tpm_measure_path(uint32_t pcr, uint32_t event_type,
                      const char *desc_prefix, const char *path) {
    if (!measured_boot || path == NULL) {
        return;
    }

    // The hash follows the last '#', as uri.c reads it; the path may hold more.
    const char *hash_sep = strrchr(path, '#');
    size_t path_len = hash_sep != NULL
        ? (size_t)(hash_sep - path)
        : strlen(path);

    // Static scratch matches uri.c's URI_BUF_SIZE; URIs longer than that
    // already panic in uri_resolve(), so a too-long path here is a bug.
    static char stripped[4096];
    if (path_len >= sizeof(stripped)) {
        return;
    }
    memcpy(stripped, path, path_len);
    stripped[path_len] = '\0';

    tpm_measure(pcr, event_type, stripped, path_len, desc_prefix, stripped);
}

static size_t log_memory_extent(uintptr_t address) {
    // Until exit, the saved map predates any late firmware allocations.
    return tpm_log_extent(
        efi_boot_services_exited ? NULL : gBS, efi_mmap, efi_mmap_size, efi_desc_size, address
    );
}

// Confines reads of firmware-supplied data to the extent it may occupy.
struct log_reader {
    const uint8_t *data;
    size_t size;
    size_t pos;
};

static bool log_skip(struct log_reader *r, size_t count) {
    if (count > r->size - r->pos) {
        return false;
    }
    r->pos += count;
    return true;
}

static bool log_read(struct log_reader *r, void *out, size_t count) {
    const uint8_t *src = r->data + r->pos;
    if (!log_skip(r, count)) {
        return false;
    }
    memcpy(out, src, count);
    return true;
}

// Index of algorithm_id among sizes, or -1.
static int tcg_digest_index(const struct tcg_digest_sizes *sizes, uint16_t algorithm_id) {
    for (uint32_t i = 0; i < sizes->count; i++) {
        if (sizes->algorithms[i].algorithm_id == algorithm_id) {
            return i;
        }
    }
    return -1;
}

static uint32_t tcg_algorithm_bank(uint16_t algorithm_id, uint16_t digest_size) {
    switch (algorithm_id) {
        case 0x0004: {
            return digest_size == 20 ? EFI_TCG2_BOOT_HASH_ALG_SHA1 : 0;
        }
        case 0x000b: {
            return digest_size == 32 ? EFI_TCG2_BOOT_HASH_ALG_SHA256 : 0;
        }
        case 0x000c: {
            return digest_size == 48 ? EFI_TCG2_BOOT_HASH_ALG_SHA384 : 0;
        }
        case 0x000d: {
            return digest_size == 64 ? EFI_TCG2_BOOT_HASH_ALG_SHA512 : 0;
        }
        case 0x0012: {
            return digest_size == 32 ? EFI_TCG2_BOOT_HASH_ALG_SM3_256 : 0;
        }
        case 0x0027: {
            return digest_size == 32 ? EFI_TCG2_BOOT_HASH_ALG_SHA3_256 : 0;
        }
        case 0x0028: {
            return digest_size == 48 ? EFI_TCG2_BOOT_HASH_ALG_SHA3_384 : 0;
        }
        case 0x0029: {
            return digest_size == 64 ? EFI_TCG2_BOOT_HASH_ALG_SHA3_512 : 0;
        }
        default: {
            return 0;
        }
    }
}

// The record size functions return 0 for a record that is malformed or does
// not fit in the size bytes at data.

static size_t tcg_pcr_event_size(const void *data, size_t size) {
    struct log_reader r = { data, size, 0 };
    struct tcg_pcr_event event;
    if (!log_read(&r, &event, sizeof(event)) || !log_skip(&r, event.event_size)) {
        return 0;
    }
    return r.pos;
}

static size_t tcg_pcr_event2_size(
    const void *data,
    size_t size,
    const struct tcg_digest_sizes *sizes,
    const struct efi_tcg2_event *expected,
    bool *found
) {
    struct log_reader r = { data, size, 0 };

    struct tcg_pcr_event2_head head;
    if (!log_read(&r, &head, sizeof(struct tcg_pcr_event2_head))
     || head.digest_count > sizes->count
     || (tpm_secure_boot_available() && head.event_type != TPM_EV_NO_ACTION
      && (head.digest_count == 0 || head.pcr_index > 23))) {
        return 0;
    }

    _Static_assert(TCG_LOG_MAX_ALGORITHMS <= 32, "one bit of seen per algorithm");
    uint32_t seen = 0;
    uint32_t banks = 0;
    for (uint32_t i = 0; i < head.digest_count; i++) {
        uint16_t algorithm_id;
        if (!log_read(&r, &algorithm_id, sizeof(uint16_t))) {
            return 0;
        }
        int index = tcg_digest_index(sizes, algorithm_id);
        if (index < 0 || (seen & ((uint32_t)1 << index)) != 0) {
            return 0;
        }
        seen |= (uint32_t)1 << index;
        banks |= tcg_algorithm_bank(algorithm_id, sizes->algorithms[index].digest_size);
        if (!log_skip(&r, sizes->algorithms[index].digest_size)) {
            return 0;
        }
    }
    if (tpm_secure_boot_available() && head.event_type != TPM_EV_NO_ACTION
     && (banks & required_pcr_banks) != required_pcr_banks) {
        return 0;
    }

    uint32_t event_size;
    if (!log_read(&r, &event_size, sizeof(uint32_t)) || !log_skip(&r, event_size)) {
        return 0;
    }
    if (expected != NULL && head.pcr_index == expected->header.pcr_index
     && head.event_type == expected->header.event_type
     && event_size == expected->size - offsetof(struct efi_tcg2_event, event)
     && memcmp(r.data + (r.pos - event_size), expected->event, event_size) == 0) {
        *found = true;
    }
    return r.pos;
}

// Also fills sizes from the header's digestSizes.
static size_t tcg_log_header_size(const void *data, size_t size,
                                  struct tcg_digest_sizes *sizes) {
    size_t header_size = tcg_pcr_event_size(data, size);
    if (header_size == 0) {
        return 0;
    }

    if (tpm_secure_boot_available()) {
        struct tcg_pcr_event first;
        static const uint8_t zero_digest[20];
        memcpy(&first, data, sizeof(struct tcg_pcr_event));
        if (first.pcr_index != 0 || first.event_type != TPM_EV_NO_ACTION
         || memcmp(first.digest, zero_digest, sizeof(zero_digest)) != 0) {
            return 0;
        }
    }

    struct log_reader r = {
        (const uint8_t *)data + sizeof(struct tcg_pcr_event),
        header_size - sizeof(struct tcg_pcr_event),
        0
    };

    struct tcg_efi_spec_id_event spec_id;
    if (!log_read(&r, &spec_id, sizeof(struct tcg_efi_spec_id_event))
     || memcmp(spec_id.signature, "Spec ID Event03", sizeof(spec_id.signature)) != 0) {
        return 0;
    }
    if (tpm_secure_boot_available()
     && (spec_id.spec_version_major != 2
      || (spec_id.uintn_size != 1 && spec_id.uintn_size != 2))) {
        return 0;
    }
    if (spec_id.number_of_algorithms == 0
     || spec_id.number_of_algorithms > TCG_LOG_MAX_ALGORITHMS) {
        return 0;
    }

    sizes->count = 0;
    for (uint32_t i = 0; i < spec_id.number_of_algorithms; i++) {
        struct tcg_efi_spec_id_event_algorithm_size algorithm;
        if (!log_read(&r, &algorithm, sizeof(struct tcg_efi_spec_id_event_algorithm_size))
         || algorithm.digest_size == 0) {
            return 0;
        }
        // A repeated algorithm would leave its digest size ambiguous.
        if (tcg_digest_index(sizes, algorithm.algorithm_id) >= 0) {
            return 0;
        }
        sizes->algorithms[sizes->count++] = algorithm;
    }

    uint8_t vendor_info_size;
    if (!log_read(&r, &vendor_info_size, sizeof(uint8_t))
     || !log_skip(&r, vendor_info_size)) {
        return 0;
    }
    if (tpm_secure_boot_available() && r.pos != r.size) {
        return 0;
    }

    return header_size;
}

// Size of the log at data whose last record starts last_offset bytes in, or 0
// unless its records lead exactly there and the last one ends within limit.
// last_offset must be below limit. For a crypto-agile log, also fills sizes.
static size_t tcg_log_size(const uint8_t *data, size_t limit, size_t last_offset,
                           uint32_t format, struct tcg_digest_sizes *sizes, size_t previous_size,
                           const struct efi_tcg2_event *expected, bool *found) {
    size_t offset = 0;

    for (;;) {
        // A record ahead of the last one cannot reach past its start.
        size_t avail = offset < last_offset ? last_offset - offset : limit - offset;

        size_t size;
        if (format == EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2) {
            size = tcg_pcr_event_size(data + offset, avail);
        } else if (offset == 0) {
            size = tcg_log_header_size(data, avail, sizes);
        } else {
            size = tcg_pcr_event2_size(
                data + offset, avail, sizes, offset >= previous_size ? expected : NULL, found
            );
        }

        if (size == 0) {
            return 0;
        }
        if (offset == last_offset) {
            return offset + size;
        }
        offset += size;
    }
}

static size_t tpm_check_log(
    size_t previous_size,
    const struct efi_tcg2_event *expected,
    bool after_load_image
) {
    efi_physical_address location = 0;
    efi_physical_address last_entry = 0;
    bool truncated = false;
    efi_status status = tcg2->get_event_log(tcg2, EFI_TCG2_EVENT_LOG_FORMAT_TCG_2,
        &location, &last_entry, &truncated);
    if (status != EFI_SUCCESS || (truncated && !tpm_log_truncated && !after_load_image)
     || location == 0 || last_entry == 0 || (uintptr_t)location != location
     || last_entry < location) {
        panic(false, "tpm: authenticated boot requires a complete event log");
    }

    size_t limit = MIN((size_t)TCG_LOG_MAX_SIZE, log_memory_extent((uintptr_t)location));
    if (last_entry - location >= limit) {
        panic(false, "tpm: event log last entry is out of range");
    }
    struct tcg_digest_sizes sizes = { 0 };
    bool found = false;
    size_t size = tcg_log_size(
        (const uint8_t *)(uintptr_t)location, limit, last_entry - location,
        EFI_TCG2_EVENT_LOG_FORMAT_TCG_2, &sizes, previous_size, expected, &found
    );
    if (size == 0) {
        panic(false, "tpm: authenticated boot event log is malformed");
    }

    uint32_t banks = 0;
    for (uint32_t i = 0; i < sizes.count; i++) {
        banks |= tcg_algorithm_bank(sizes.algorithms[i].algorithm_id,
                                    sizes.algorithms[i].digest_size);
    }
    if ((banks & required_pcr_banks) != required_pcr_banks) {
        panic(false, "tpm: authenticated boot event log is missing active PCR banks");
    }

    if (expected != NULL && !found) {
        panic(false, "tpm: Measurement has no event-log evidence");
    }
    if (truncated && after_load_image) {
        tpm_log_truncated = true;
        tpm_log_full();
    }
    return size;
}

void tpm_validate_log(bool after_load_image) {
    if (tpm_secure_boot_available()) {
        tpm_check_log(0, NULL, after_load_image);
    }
}

static bool final_events_reader(struct log_reader *r, uint64_t *count) {
    const void *table = tpm_get_final_events_table();
    *r = (struct log_reader){ table, log_memory_extent((uintptr_t)table), 0 };
    if (table == NULL) {
        return false;
    }
    struct efi_tcg2_final_events_table header;
    if (!log_read(r, &header, offsetof(struct efi_tcg2_final_events_table, event))
     || header.version != TCG_FINAL_EVENTS_TABLE_VERSION) {
        return false;
    }
    *count = header.number_of_events;
    return true;
}

// The final events table repeats every event logged since the first
// GetEventLog call (TCG EFI Protocol section 7, UEFI 2.11 section 38.3), so the
// events it holds at capture are the newest ones of the captured log. Gives
// their number and size, unless that cannot be established: an OS then
// replays them all, and a duplicated event does less harm than a lost one.
static bool tcg_final_events_preboot(const uint8_t *log_data, size_t log_size,
    const struct tcg_digest_sizes *sizes, uint64_t *count, size_t *total_size) {
    struct log_reader r;
    uint64_t entries;
    if (!final_events_reader(&r, &entries)) {
        return false;
    }
    const uint8_t *events = r.data + r.pos;
    size_t limit = log_size - tcg_pcr_event_size(log_data, log_size);
    limit = MIN(limit, r.size - r.pos);
    size_t total = 0;

    for (uint64_t i = 0; i < entries; i++) {
        size_t size = tcg_pcr_event2_size(events + total, limit - total, sizes, NULL, NULL);
        if (size == 0) {
            return false;
        }
        total += size;
    }

    if (memcmp(events, log_data + log_size - total, total) != 0) {
        return false;
    }

    *count = entries;
    *total_size = total;
    return true;
}

static bool tcg2_get_event_log(uint32_t *format, efi_physical_address *location,
    efi_physical_address *last_entry, bool *truncated) {
    efi_tcg2_event_log_bitmap supported =
        EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2 | EFI_TCG2_EVENT_LOG_FORMAT_TCG_2;

    struct efi_tcg2_boot_service_capability cap;
    memset(&cap, 0, sizeof(cap));
    cap.size = sizeof(cap);
    if (tcg2->get_capability(tcg2, &cap) == EFI_SUCCESS) {
        supported = cap.supported_event_logs;
    }

    // The crypto-agile log records every active PCR bank, and the final events
    // table only comes in that format (TCG EFI Protocol sections 5.2 and 7).
    static const efi_tcg2_event_log_format formats[] = {
        EFI_TCG2_EVENT_LOG_FORMAT_TCG_2,
        EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2,
    };

    for (size_t i = 0; i < SIZEOF_ARRAY(formats); i++) {
        if ((supported & formats[i]) == 0) {
            continue;
        }
        efi_status status = tcg2->get_event_log(tcg2, formats[i], location,
                                              last_entry, truncated);
        if (status == EFI_SUCCESS) {
            *format = formats[i];
            return true;
        }
        printv("tpm: GetEventLog for log format %u failed: %X\n",
               (uint32_t)formats[i], (uint64_t)status);
    }

    return false;
}

#define EVENT_LOG_UNCAPTURED 0
#define EVENT_LOG_CAPTURED 1
#define EVENT_LOG_UNAVAILABLE 2

static int event_log_state = EVENT_LOG_UNCAPTURED;
static uint32_t event_log_format;
static void *event_log_copy;
static size_t event_log_size;
static size_t event_log_capacity;
static size_t event_log_preboot_size;

// Set where the log is known to end with the first event_log_preboot_count
// events of the final events table, which take event_log_preboot_size bytes.
static bool event_log_appendable;
static uint64_t event_log_preboot_count;
static struct tcg_digest_sizes event_log_digest_sizes;

static bool event_log_capture(void) {
    uint32_t format;
    efi_physical_address location = 0;
    efi_physical_address last_entry = 0;
    bool truncated = false;

    if (tcg2 != NULL) {
        if (!tcg2_get_event_log(&format, &location, &last_entry, &truncated)) {
            printv("tpm: unable to retrieve the event log\n");
            return false;
        }
    } else if (cc != NULL) {
        format = EFI_CC_EVENT_LOG_FORMAT_TCG_2;
        efi_status status = cc->get_event_log(cc, format, &location, &last_entry, &truncated);
        if (status != EFI_SUCCESS) {
            printv("tpm: CC GetEventLog failed: %X\n", (uint64_t)status);
            return false;
        }
    } else {
        return false;
    }

    if (location == 0 || (uintptr_t)location != location) {
        printv("tpm: firmware returned no usable event log\n");
        return false;
    }

    const uint8_t *log_data = (const uint8_t *)(uintptr_t)location;
    size_t limit = MIN((size_t)TCG_LOG_MAX_SIZE, log_memory_extent((uintptr_t)location));
    struct tcg_digest_sizes sizes = { 0 };
    size_t size = 0;

    // A last entry of 0 stands for a log with no events (TCG EFI Protocol 6.5.3).
    if (last_entry != 0) {
        if (last_entry < location || last_entry - location >= limit) {
            printv("tpm: event log last entry %X is out of range\n", (uint64_t)last_entry);
            return false;
        }
        size = tcg_log_size(log_data, limit, last_entry - location, format, &sizes, 0, NULL, NULL);
        if (size == 0) {
            printv("tpm: event log is malformed or too large\n");
            return false;
        }
    }

    void *copy = NULL;
    size_t capacity = size;
    if (size != 0) {
        if (format == EFI_TCG2_EVENT_LOG_FORMAT_TCG_2) {
            capacity += TCG_LOG_HEADROOM;
        }
        copy = ext_mem_alloc(capacity);
        memcpy(copy, log_data, size);
    }

    // A truncated log can lack events that the final events table holds.
    size_t preboot_size = 0;
    uint64_t preboot_count = 0;
    bool appendable = false;
    if (truncated) {
        tpm_log_full();
    } else if (format == EFI_TCG2_EVENT_LOG_FORMAT_TCG_2 && size != 0) {
        appendable = tcg_final_events_preboot(copy, size, &sizes,
            &preboot_count, &preboot_size);
    }

    event_log_format = format;
    event_log_copy = copy;
    event_log_size = size;
    event_log_capacity = capacity;
    event_log_preboot_size = preboot_size;
    event_log_appendable = appendable;
    event_log_preboot_count = preboot_count;
    event_log_digest_sizes = sizes;

    printv("tpm: captured %U byte event log, format %u, final events pre-boot size %U\n",
           (uint64_t)size, format, (uint64_t)preboot_size);
    return true;
}

bool tpm_get_event_log(uint32_t *format, void **address, size_t *size) {
    if (event_log_state == EVENT_LOG_UNCAPTURED) {
        event_log_state = event_log_capture() ? EVENT_LOG_CAPTURED : EVENT_LOG_UNAVAILABLE;
    }

    if (event_log_state != EVENT_LOG_CAPTURED) {
        return false;
    }

    *format = event_log_format;
    *address = event_log_copy;
    *size = event_log_size;
    return true;
}

size_t tpm_append_final_events(void) {
    if (!event_log_appendable) {
        return event_log_size;
    }

    struct log_reader r;
    uint64_t entries;
    if (!final_events_reader(&r, &entries) || entries < event_log_preboot_count
     || !log_skip(&r, event_log_preboot_size)) {
        return event_log_size;
    }
    const uint8_t *events = r.data + r.pos;
    size_t room = MIN(event_log_capacity - event_log_size, r.size - r.pos);
    size_t total = 0;

    for (uint64_t i = event_log_preboot_count; i < entries; i++) {
        size_t size = tcg_pcr_event2_size(events + total, room - total,
            &event_log_digest_sizes, NULL, NULL);
        if (size == 0) {
            printv("tpm: cannot append the events logged after the capture\n");
            return event_log_size;
        }
        total += size;
    }

    memcpy((uint8_t *)event_log_copy + event_log_size, events, total);
    event_log_size += total;
    event_log_preboot_size += total;
    event_log_preboot_count = entries;

    printv("tpm: appended %U bytes of events logged after the capture\n", (uint64_t)total);
    return event_log_size;
}

size_t tpm_get_final_events_preboot_size(void) {
    if (event_log_state != EVENT_LOG_CAPTURED) {
        return 0;
    }
    return event_log_preboot_size;
}

void tpm_release_event_log(void) {
    if (event_log_copy != NULL) {
        pmm_free(event_log_copy, event_log_capacity);
    }

    event_log_copy = NULL;
    event_log_size = 0;
    event_log_capacity = 0;
    event_log_preboot_size = 0;
    event_log_appendable = false;
    event_log_state = EVENT_LOG_UNAVAILABLE;
}

void *tpm_get_final_events_table(void) {
    struct efi_guid guid;
    if (tcg2 != NULL) {
        struct efi_guid tcg2_guid = EFI_TCG2_FINAL_EVENTS_TABLE_GUID;
        guid = tcg2_guid;
    } else if (cc != NULL) {
        struct efi_guid cc_guid = EFI_CC_FINAL_EVENTS_TABLE_GUID;
        guid = cc_guid;
    } else {
        return NULL;
    }

    for (size_t i = 0; i < gST->number_of_table_entries; i++) {
        if (memcmp(&gST->configuration_table[i].vendor_guid,
                   &guid, sizeof(struct efi_guid)) == 0) {
            return gST->configuration_table[i].vendor_table;
        }
    }
    return NULL;
}

#endif
