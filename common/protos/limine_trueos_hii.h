#ifndef PROTOS__LIMINE_TRUEOS_HII_H__
#define PROTOS__LIMINE_TRUEOS_HII_H__

#include <stdint.h>

// Experimental, opt-in, TRUEOS-only extension to the Limine boot protocol.
// Not part of the upstream limine-protocol specification: the two custom
// words below are locally chosen, not registered anywhere upstream.
//
// Kernels that never declare a matching request struct never trigger the
// capture in the first place (see get_request() in protos/limine.c), so
// this costs nothing on firmware/kernels that don't ask for it.
#define LIMINE_TRUEOS_HII_CAPTURE_REQUEST_ID \
    { LIMINE_COMMON_MAGIC, 0x1e2f9b6a7c4d5e81, 0x9a0b3c7d2e6f5148 }

struct limine_trueos_hii_capture_response {
    uint64_t revision;
    // TRPAY1-formatted payload; see tools/firmware-scout/README.md and
    // src/shell2/cmds/bios_capture.rs for the section-directory layout.
    LIMINE_PTR(void *) address;
    uint64_t size;
};

struct limine_trueos_hii_capture_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_trueos_hii_capture_response *) response;
};

#endif
