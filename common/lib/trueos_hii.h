#ifndef LIB__TRUEOS_HII_H__
#define LIB__TRUEOS_HII_H__

#if defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Experimental, TRUEOS-only extension to the Limine boot protocol: an
// opt-in, read-only capture of the firmware's HII package lists and current
// HII configuration, performed while Boot Services are still callable (right
// before ExitBootServices()). Not part of the upstream limine-protocol spec.
//
// The captured payload uses the same TRPAY1 section-directory format that
// FirmwareScout.efi's independent preboot capture already produces, so the
// TRUEOS kernel-side decoder can read either source unchanged.
//
// On success, *out_address/*out_size describe an ext_mem_alloc'd buffer that
// remains valid in the memory map handed to the kernel. Returns false if the
// firmware exposes neither the HII database nor HII config routing
// protocol; a capture-status section is still worth publishing in that case
// only if at least one of the two protocols was locatable.
bool trueos_hii_capture(void **out_address, size_t *out_size);

#endif

#endif
