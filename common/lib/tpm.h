#ifndef LIB__TPM_H__
#define LIB__TPM_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// PCR allocation, following the GRUB convention:
//   PCR 8: kernel command line and other authoritative strings
//   PCR 9: kernel image, initrd, devicetree, and other binary blobs
#define TPM_PCR_BOOT_AUTH       8
#define TPM_PCR_LOADED_IMAGES   9

// TCG PC Client Platform Firmware Profile event types
#define TPM_EV_IPL              0x0000000d

#if defined (UEFI)

void tpm_init(void);

// True if a TCG2 or CC measurement protocol is available.
bool tpm_present(void);

void tpm_measure(uint32_t pcr, uint32_t event_type,
                 const void *data, size_t data_size,
                 const char *desc_prefix, const char *desc_value);

// Measure a config-supplied URI string into the given PCR with any trailing
// `#<hash>` suffix stripped, so the digest captures only the policy-stable
// portion (resource, root, path, and any `$` decompression marker). The
// event description shows the same stripped string.
void tpm_measure_path(uint32_t pcr, uint32_t event_type,
                      const char *desc_prefix, const char *path);

// Capture the firmware TCG2 event log into bootloader-reclaimable memory
// and expose the raw event stream. `format` receives the TCG event log
// format identifier (1 = TCG 1.2, 2 = TCG 2.0 crypto-agile). Returns false
// if no TPM is present or capture failed.
bool tpm_get_event_log(uint32_t *format, void **address, size_t *size);

// Free the captured event log buffer. Subsequent tpm_get_event_log() calls
// return false.
void tpm_release_event_log(void);

// Compute the in-memory size of one TCG 2.0 crypto-agile event entry.
// `header` must point to the spec-ID event at the start of the log; it
// carries the per-algorithm digest sizes needed to walk variable-length
// digest lists. Returns 0 on malformed input.
uint32_t tpm_calc_event_size(const void *event, const void *header);

// Locate the firmware's final-events table for the active measurement
// protocol (EFI_TCG2_FINAL_EVENTS_TABLE_GUID for TCG2, or
// EFI_CC_FINAL_EVENTS_TABLE_GUID for CC). Both tables share the same
// {Version, NumberOfEvents, Events[]} layout. Returns NULL if no TPM/CC
// interface is active or the table isn't present.
void *tpm_get_final_events_table(void);

#endif

#endif
