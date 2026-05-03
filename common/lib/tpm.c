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
#include <mm/pmm.h>

// TCG event log entry layouts (TCG PC Client Platform Firmware Profile).
struct tpm_pcr_event_v1_2 {
    uint32_t pcr_idx;
    uint32_t event_type;
    uint8_t  digest[20];
    uint32_t event_size;
    uint8_t  event[];
} __attribute__((packed));

struct tpm_specid_event_alg {
    uint16_t alg_id;
    uint16_t digest_size;
} __attribute__((packed));

struct tpm_specid_event_head {
    uint8_t  signature[16];
    uint32_t platform_class;
    uint8_t  spec_version_minor;
    uint8_t  spec_version_major;
    uint8_t  spec_errata;
    uint8_t  uintn_size;
    uint32_t num_algs;
    struct tpm_specid_event_alg digest_sizes[];
} __attribute__((packed));

// Followed by `count` digests (uint16_t alg_id + variable-length digest),
// then a uint32_t event_size and event_size bytes of event data.
struct tpm_pcr_event2_head {
    uint32_t pcr_idx;
    uint32_t event_type;
    uint32_t count;
} __attribute__((packed));

#define TCG_EV_NO_ACTION 3
#define TCG_SPECID_SIG   "Spec ID Event03"

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
            printv("tpm: HashLogExtendEvent for PCR %u failed: %X\n",
                   pcr, (uint64_t)status);
        }

        pmm_free(event, event_size);
    } else if (cc != NULL) {
        // CC platforms expose Memory Reference (MR) registers rather than
        // PCRs. The protocol provides a translation from a requested PCR
        // index to the platform's corresponding MR index.
        EFI_CC_MR_INDEX mr_index;
        EFI_STATUS status = cc->MapPcrToMrIndex(cc, pcr, &mr_index);
        if (status != EFI_SUCCESS) {
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
            printv("tpm: CC HashLogExtendEvent for PCR %u (MR %u) failed: %X\n",
                   pcr, (uint32_t)mr_index, (uint64_t)status);
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

uint32_t tpm_calc_event_size(const void *event_p, const void *header_p) {
    const struct tpm_pcr_event2_head *event = event_p;
    const struct tpm_pcr_event_v1_2 *event_header = header_p;

    static const uint8_t zero_digest[20] = {0};

    if (event_header->pcr_idx != 0
     || event_header->event_type != TCG_EV_NO_ACTION
     || memcmp(event_header->digest, zero_digest, sizeof(zero_digest)) != 0) {
        return 0;
    }

    const struct tpm_specid_event_head *efispecid =
        (const struct tpm_specid_event_head *)event_header->event;

    if (memcmp(efispecid->signature, TCG_SPECID_SIG, sizeof(TCG_SPECID_SIG)) != 0
     || efispecid->num_algs == 0) {
        return 0;
    }

    const uint8_t *marker_start = (const uint8_t *)event_p;
    const uint8_t *marker = marker_start
                          + sizeof(event->pcr_idx)
                          + sizeof(event->event_type)
                          + sizeof(event->count);

    for (uint32_t i = 0; i < event->count; i++) {
        uint16_t halg;
        memcpy(&halg, marker, sizeof(halg));
        marker += sizeof(halg);

        uint32_t j;
        for (j = 0; j < efispecid->num_algs; j++) {
            if (halg == efispecid->digest_sizes[j].alg_id) {
                marker += efispecid->digest_sizes[j].digest_size;
                break;
            }
        }
        if (j == efispecid->num_algs) {
            return 0;
        }
    }

    uint32_t trailing_event_size;
    memcpy(&trailing_event_size, marker, sizeof(trailing_event_size));
    marker += sizeof(trailing_event_size) + trailing_event_size;

    if (event->event_type == 0 && trailing_event_size == 0) {
        return 0;
    }

    return (uint32_t)(marker - marker_start);
}

static void *captured_log = NULL;
static size_t captured_log_size = 0;
static uint32_t captured_log_format = 0;
static bool capture_attempted = false;

// Pull the firmware event log via GetEventLog and copy the raw event bytes
// into a bootloader-reclaimable buffer. Idempotent. Returns true if the
// captured state is valid.
static bool tpm_capture_event_log(void) {
    if (capture_attempted) {
        return captured_log != NULL;
    }
    capture_attempted = true;

    if (tcg2 == NULL && cc == NULL) {
        return false;
    }

    EFI_PHYSICAL_ADDRESS log_location = 0, log_last_entry = 0;
    BOOLEAN truncated = FALSE;
    uint32_t log_format = EFI_TCG2_EVENT_LOG_FORMAT_TCG_2;
    EFI_STATUS status;

    if (tcg2 != NULL) {
        status = tcg2->GetEventLog(tcg2, log_format,
            &log_location, &log_last_entry, &truncated);
        if (status != EFI_SUCCESS || log_location == 0) {
            log_format = EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2;
            status = tcg2->GetEventLog(tcg2, log_format,
                &log_location, &log_last_entry, &truncated);
            if (status != EFI_SUCCESS || log_location == 0) {
                return false;
            }
        }
    } else {
        // CC measurement protocol. Only the TCG 2.0 log format is defined.
        log_format = EFI_CC_EVENT_LOG_FORMAT_TCG_2;
        status = cc->GetEventLog(cc, log_format,
            &log_location, &log_last_entry, &truncated);
        if (status != EFI_SUCCESS || log_location == 0) {
            return false;
        }
    }

    uint32_t log_size = 0;
    if (log_last_entry != 0) {
        uint32_t last_entry_size = 0;
        // The first entry of a TCG 2.0 log is itself a v1.2-format spec-ID
        // event; only entries after it follow the crypto-agile layout.
        if (log_format > EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2
         && log_last_entry != log_location) {
            last_entry_size = tpm_calc_event_size(
                (void *)(uintptr_t)log_last_entry,
                (void *)(uintptr_t)log_location);
        } else {
            const struct tpm_pcr_event_v1_2 *e =
                (const struct tpm_pcr_event_v1_2 *)(uintptr_t)log_last_entry;
            last_entry_size = sizeof(struct tpm_pcr_event_v1_2) + e->event_size;
        }
        log_size = (uint32_t)(log_last_entry - log_location) + last_entry_size;
    }

    void *log_bytes = NULL;
    if (log_size > 0) {
        log_bytes = ext_mem_alloc(log_size);
        memcpy(log_bytes, (void *)(uintptr_t)log_location, log_size);
    }

    captured_log = log_bytes;
    captured_log_size = log_size;
    captured_log_format = log_format;
    return true;
}

bool tpm_get_event_log(uint32_t *format, void **address, size_t *size) {
    if (!tpm_capture_event_log()) {
        return false;
    }

    *format = captured_log_format;
    *address = captured_log;
    *size = captured_log_size;
    return true;
}

void tpm_release_event_log(void) {
    if (captured_log != NULL) {
        pmm_free(captured_log, captured_log_size);
        captured_log = NULL;
    }
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
