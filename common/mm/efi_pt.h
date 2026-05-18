#ifndef MM__EFI_PT_H__
#define MM__EFI_PT_H__

#include <stdint.h>

#if defined (__x86_64__) && defined (UEFI)

void efi_pt_set_fb_wc(uint64_t base, uint64_t size);
void efi_pt_restore(void);

#endif

#endif
