#ifndef PROTOS__LINUX_EFI_H
#define PROTOS__LINUX_EFI_H

#include <stdbool.h>

#if defined(UEFI)
void linux_efi_prepare(char *config, const char *cmdline, bool linux_protocol);
void linux_efi_register(void);
void linux_efi_release(void);
#endif

#endif
