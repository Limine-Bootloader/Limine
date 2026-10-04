#if defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <efi.h>
#include <efi/protocol/efitcg2.h>
#include <efi/protocol/eficc.h>
#include <lib/tpm.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <lib/libc.h>
#include <lib/getchar.h>
#include <mm/pmm.h>

// Event log records, as laid out by the TCG EFI Protocol Specification, Family
// "2.0", Level 00 Revision 00.13. They are densely packed, and little-endian
// like every Limine target (section 3.1).

// Real logs are far smaller; a larger one is taken to be corrupt.
#define TCG_LOG_MAX_SIZE 0x400000

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

// At most one of these is non-NULL after tpm_init. tcg2 takes precedence
// since it's the more common case (real TPMs); the cc fallback is for
// confidential-computing platforms (TDX, SEV-SNP) without a discrete TPM.
static EFI_TCG2_PROTOCOL *tcg2 = NULL;
static EFI_CC_MEASUREMENT_PROTOCOL *cc = NULL;

void tpm_init(void) {
    EFI_GUID tcg2_guid = EFI_TCG2_PROTOCOL_GUID;
    EFI_TCG2_PROTOCOL *tcg2_proto = NULL;
    EFI_STATUS status = gBS->LocateProtocol(&tcg2_guid, NULL, (void **)&tcg2_proto);
    if (status == EFI_SUCCESS && tcg2_proto != NULL) {
        EFI_TCG2_BOOT_SERVICE_CAPABILITY cap;
        memset(&cap, 0, sizeof(cap));
        cap.Size = sizeof(cap);
        status = tcg2_proto->GetCapability(tcg2_proto, &cap);
        if (status == EFI_SUCCESS && cap.TPMPresentFlag) {
            tcg2 = tcg2_proto;
            printv("tpm: TCG2 protocol located, TPM present (active PCR banks: %x)\n",
                   (uint32_t)cap.ActivePcrBanks);
            return;
        }
    }

    // No TCG2/TPM 2.0; fall back to the CC measurement protocol.
    EFI_GUID cc_guid = EFI_CC_MEASUREMENT_PROTOCOL_GUID;
    EFI_CC_MEASUREMENT_PROTOCOL *cc_proto = NULL;
    status = gBS->LocateProtocol(&cc_guid, NULL, (void **)&cc_proto);
    if (status == EFI_SUCCESS && cc_proto != NULL) {
        EFI_CC_BOOT_SERVICE_CAPABILITY cap;
        memset(&cap, 0, sizeof(cap));
        cap.Size = sizeof(cap);
        status = cc_proto->GetCapability(cc_proto, &cap);
        if (status == EFI_SUCCESS) {
            cc = cc_proto;
            const char *cc_name = "unknown";
            switch (cap.CcType.Type) {
                case EFI_CC_TYPE_AMD_SEV:   cc_name = "AMD SEV";   break;
                case EFI_CC_TYPE_INTEL_TDX: cc_name = "Intel TDX"; break;
            }
            printv("tpm: CC measurement protocol located (type: %s)\n", cc_name);
            return;
        }
    }
}

bool tpm_present(void) {
    return tcg2 != NULL || cc != NULL;
}

static void tpm_extend_failed(void) {
    print("         Press Y to continue, press any other key to panic...");

    char ch = getchar();
    print("\n");
    if (ch != 'Y' && ch != 'y') {
        panic(false, "tpm: refusing to boot with an unmeasured component");
    }
}

void tpm_measure(uint32_t pcr, uint32_t event_type,
                 const void *data, size_t data_size,
                 const char *desc_prefix, const char *desc_value) {
    if (!measured_boot || data == NULL) {
        return;
    }

    size_t prefix_len = desc_prefix != NULL ? strlen(desc_prefix) : 0;
    size_t value_len = desc_value != NULL ? strlen(desc_value) : 0;
    size_t desc_len = prefix_len + value_len + 1;

    if (tcg2 != NULL) {
        size_t event_size = offsetof(EFI_TCG2_EVENT, Event) + desc_len;

        EFI_TCG2_EVENT *event = ext_mem_alloc(event_size);
        event->Size = (UINT32)event_size;
        event->Header.HeaderSize = sizeof(EFI_TCG2_EVENT_HEADER);
        event->Header.HeaderVersion = 1;
        event->Header.PCRIndex = pcr;
        event->Header.EventType = event_type;
        if (prefix_len > 0) {
            memcpy(event->Event, desc_prefix, prefix_len);
        }
        if (value_len > 0) {
            memcpy(event->Event + prefix_len, desc_value, value_len);
        }

        EFI_STATUS status = tcg2->HashLogExtendEvent(
            tcg2, 0,
            (EFI_PHYSICAL_ADDRESS)(uintptr_t)data, (UINT64)data_size,
            event);
        if (status != EFI_SUCCESS) {
            quiet = false;
            print("WARNING: tpm: HashLogExtendEvent for PCR %u failed: %X\n"
                  "         This component has not been measured.\n",
                  pcr, (uint64_t)status);
            tpm_extend_failed();
        }

        pmm_free(event, event_size);
    } else if (cc != NULL) {
        // CC platforms expose Memory Reference (MR) registers rather than
        // PCRs. The protocol provides a translation from a requested PCR
        // index to the platform's corresponding MR index.
        EFI_CC_MR_INDEX mr_index;
        EFI_STATUS status = cc->MapPcrToMrIndex(cc, pcr, &mr_index);
        if (status != EFI_SUCCESS) {
            quiet = false;
            print("WARNING: tpm: no measurement register for PCR %u: %X\n"
                  "         This component has not been measured.\n",
                  pcr, (uint64_t)status);
            tpm_extend_failed();
            return;
        }

        size_t event_size = offsetof(EFI_CC_EVENT, Event) + desc_len;

        EFI_CC_EVENT *event = ext_mem_alloc(event_size);
        event->Size = (UINT32)event_size;
        event->Header.HeaderSize = sizeof(EFI_CC_EVENT_HEADER);
        event->Header.HeaderVersion = EFI_CC_EVENT_HEADER_VERSION;
        event->Header.MrIndex = mr_index;
        event->Header.EventType = event_type;
        if (prefix_len > 0) {
            memcpy(event->Event, desc_prefix, prefix_len);
        }
        if (value_len > 0) {
            memcpy(event->Event + prefix_len, desc_value, value_len);
        }

        status = cc->HashLogExtendEvent(
            cc, 0,
            (EFI_PHYSICAL_ADDRESS)(uintptr_t)data, (UINT64)data_size,
            event);
        if (status != EFI_SUCCESS) {
            quiet = false;
            print("WARNING: tpm: CC HashLogExtendEvent for PCR %u (MR %u) failed: %X\n"
                  "         This component has not been measured.\n",
                  pcr, (uint32_t)mr_index, (uint64_t)status);
            tpm_extend_failed();
        }

        pmm_free(event, event_size);
    }
}

void tpm_measure_path(uint32_t pcr, uint32_t event_type,
                      const char *desc_prefix, const char *path) {
    if (!measured_boot || path == NULL) {
        return;
    }

    const char *hash_sep = strchr(path, '#');
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

static size_t tcg_pcr_event2_size(const void *data, size_t size,
                                  const struct tcg_digest_sizes *sizes) {
    struct log_reader r = { data, size, 0 };

    // Section 5.3 wants a digest for each algorithm of the header. Fewer are
    // let through, as only the record's extent matters here and the OS checks
    // the log itself, but none may repeat, so there cannot be more.
    struct tcg_pcr_event2_head head;
    if (!log_read(&r, &head, sizeof(head)) || head.digest_count > sizes->count) {
        return 0;
    }

    _Static_assert(TCG_LOG_MAX_ALGORITHMS <= 32, "one bit of seen per algorithm");
    uint32_t seen = 0;
    for (uint32_t i = 0; i < head.digest_count; i++) {
        uint16_t algorithm_id;
        if (!log_read(&r, &algorithm_id, sizeof(algorithm_id))) {
            return 0;
        }
        int index = tcg_digest_index(sizes, algorithm_id);
        if (index < 0 || (seen & (UINT32_C(1) << index)) != 0) {
            return 0;
        }
        seen |= UINT32_C(1) << index;
        if (!log_skip(&r, sizes->algorithms[index].digest_size)) {
            return 0;
        }
    }

    uint32_t event_size;
    if (!log_read(&r, &event_size, sizeof(event_size)) || !log_skip(&r, event_size)) {
        return 0;
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

    struct log_reader r = {
        (const uint8_t *)data + sizeof(struct tcg_pcr_event),
        header_size - sizeof(struct tcg_pcr_event),
        0
    };

    struct tcg_efi_spec_id_event spec_id;
    if (!log_read(&r, &spec_id, sizeof(spec_id))
     || memcmp(spec_id.signature, "Spec ID Event03", sizeof(spec_id.signature)) != 0) {
        return 0;
    }
    if (spec_id.number_of_algorithms == 0
     || spec_id.number_of_algorithms > TCG_LOG_MAX_ALGORITHMS) {
        return 0;
    }

    sizes->count = 0;
    for (uint32_t i = 0; i < spec_id.number_of_algorithms; i++) {
        struct tcg_efi_spec_id_event_algorithm_size algorithm;
        if (!log_read(&r, &algorithm, sizeof(algorithm)) || algorithm.digest_size == 0) {
            return 0;
        }
        // A repeated algorithm would leave its digest size ambiguous.
        if (tcg_digest_index(sizes, algorithm.algorithm_id) >= 0) {
            return 0;
        }
        sizes->algorithms[sizes->count++] = algorithm;
    }

    uint8_t vendor_info_size;
    if (!log_read(&r, &vendor_info_size, sizeof(vendor_info_size))
     || !log_skip(&r, vendor_info_size)) {
        return 0;
    }

    return header_size;
}

// Size of the log at data whose last record starts last_offset bytes in, or 0
// unless its records lead exactly there and the last one ends within limit.
// last_offset must be below limit. For a crypto-agile log, also fills sizes.
static size_t tcg_log_size(const uint8_t *data, size_t limit, size_t last_offset,
                           uint32_t format, struct tcg_digest_sizes *sizes) {
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
            size = tcg_pcr_event2_size(data + offset, avail, sizes);
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

// The final events table repeats every event logged since the first
// GetEventLog call (TCG EFI Protocol section 7, UEFI 2.11 section 38.3), so the
// events it holds at capture are the newest ones of the captured log. Returns
// their size, or 0 where that cannot be established: an OS then replays them
// all, and a duplicated event does less harm than a lost one.
static size_t tcg_final_events_preboot_size(const uint8_t *log_data, size_t log_size,
                                            const struct tcg_digest_sizes *sizes) {
    const uint8_t *table = tpm_get_final_events_table();
    if (table == NULL) {
        return 0;
    }

    EFI_TCG2_FINAL_EVENTS_TABLE header;
    memcpy(&header, table, offsetof(EFI_TCG2_FINAL_EVENTS_TABLE, Events));
    if (header.Version != TCG_FINAL_EVENTS_TABLE_VERSION) {
        return 0;
    }

    const uint8_t *events = table + offsetof(EFI_TCG2_FINAL_EVENTS_TABLE, Events);
    // They can only be among the events that follow the log's header.
    size_t limit = log_size - tcg_pcr_event_size(log_data, log_size);
    size_t total = 0;

    for (uint64_t i = 0; i < header.NumberOfEvents; i++) {
        size_t size = tcg_pcr_event2_size(events + total, limit - total, sizes);
        if (size == 0) {
            return 0;
        }
        total += size;
    }

    if (memcmp(events, log_data + log_size - total, total) != 0) {
        return 0;
    }
    return total;
}

static bool tcg2_get_event_log(uint32_t *format, EFI_PHYSICAL_ADDRESS *location,
                               EFI_PHYSICAL_ADDRESS *last_entry, BOOLEAN *truncated) {
    EFI_TCG2_EVENT_LOG_BITMAP supported =
        EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2 | EFI_TCG2_EVENT_LOG_FORMAT_TCG_2;

    EFI_TCG2_BOOT_SERVICE_CAPABILITY cap;
    memset(&cap, 0, sizeof(cap));
    cap.Size = sizeof(cap);
    if (tcg2->GetCapability(tcg2, &cap) == EFI_SUCCESS) {
        supported = cap.SupportedEventLogs;
    }

    // The crypto-agile log records every active PCR bank, and the final events
    // table only comes in that format (TCG EFI Protocol sections 5.2 and 7).
    static const EFI_TCG2_EVENT_LOG_FORMAT formats[] = {
        EFI_TCG2_EVENT_LOG_FORMAT_TCG_2,
        EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2,
    };

    for (size_t i = 0; i < SIZEOF_ARRAY(formats); i++) {
        if ((supported & formats[i]) == 0) {
            continue;
        }
        EFI_STATUS status = tcg2->GetEventLog(tcg2, formats[i], location,
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

enum event_log_state {
    EVENT_LOG_UNCAPTURED,
    EVENT_LOG_CAPTURED,
    EVENT_LOG_UNAVAILABLE,
};

static enum event_log_state event_log_state = EVENT_LOG_UNCAPTURED;
static uint32_t event_log_format;
static void *event_log_copy;
static size_t event_log_size;
static size_t event_log_preboot_size;

static bool event_log_capture(void) {
    uint32_t format;
    EFI_PHYSICAL_ADDRESS location = 0;
    EFI_PHYSICAL_ADDRESS last_entry = 0;
    BOOLEAN truncated = FALSE;

    if (tcg2 != NULL) {
        if (!tcg2_get_event_log(&format, &location, &last_entry, &truncated)) {
            printv("tpm: unable to retrieve the event log\n");
            return false;
        }
    } else if (cc != NULL) {
        format = EFI_CC_EVENT_LOG_FORMAT_TCG_2;
        EFI_STATUS status = cc->GetEventLog(cc, format, &location, &last_entry, &truncated);
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
    size_t limit = MIN((uintptr_t)TCG_LOG_MAX_SIZE, UINTPTR_MAX - (uintptr_t)location);
    struct tcg_digest_sizes sizes = { 0 };
    size_t size = 0;

    // A last entry of 0 stands for a log with no events (TCG EFI Protocol 6.5.3).
    if (last_entry != 0) {
        if (last_entry < location || last_entry - location >= limit) {
            printv("tpm: event log last entry %X is out of range\n", (uint64_t)last_entry);
            return false;
        }
        size = tcg_log_size(log_data, limit, last_entry - location, format, &sizes);
        if (size == 0) {
            printv("tpm: event log is malformed or too large\n");
            return false;
        }
    }

    void *copy = NULL;
    if (size != 0) {
        copy = ext_mem_alloc(size);
        memcpy(copy, log_data, size);
    }

    // A truncated log can lack events that the final events table holds.
    size_t preboot_size = 0;
    if (truncated) {
        printv("tpm: firmware event log is truncated\n");
    } else if (format == EFI_TCG2_EVENT_LOG_FORMAT_TCG_2 && size != 0) {
        preboot_size = tcg_final_events_preboot_size(copy, size, &sizes);
    }

    event_log_format = format;
    event_log_copy = copy;
    event_log_size = size;
    event_log_preboot_size = preboot_size;

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

size_t tpm_get_final_events_preboot_size(void) {
    if (event_log_state != EVENT_LOG_CAPTURED) {
        return 0;
    }
    return event_log_preboot_size;
}

void tpm_release_event_log(void) {
    if (event_log_copy != NULL) {
        pmm_free(event_log_copy, event_log_size);
    }

    event_log_copy = NULL;
    event_log_size = 0;
    event_log_preboot_size = 0;
    event_log_state = EVENT_LOG_UNAVAILABLE;
}

void *tpm_get_final_events_table(void) {
    EFI_GUID guid;
    if (tcg2 != NULL) {
        EFI_GUID tcg2_guid = EFI_TCG2_FINAL_EVENTS_TABLE_GUID;
        guid = tcg2_guid;
    } else if (cc != NULL) {
        EFI_GUID cc_guid = EFI_CC_FINAL_EVENTS_TABLE_GUID;
        guid = cc_guid;
    } else {
        return NULL;
    }

    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
        if (memcmp(&gST->ConfigurationTable[i].VendorGuid,
                   &guid, sizeof(EFI_GUID)) == 0) {
            return gST->ConfigurationTable[i].VendorTable;
        }
    }
    return NULL;
}

#endif
