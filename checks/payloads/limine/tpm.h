#ifndef TPM_H
#define TPM_H

#include <stddef.h>

// Prints every event of a TCG2 (crypto agile) event log as "K tpm.event.N=..."
// lines: PCR, event type, SHA-256 digest, and the event data.
void tpm_dump_log(const void *log, size_t size);

#endif
