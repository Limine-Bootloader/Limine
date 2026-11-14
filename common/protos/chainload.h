#ifndef PROTOS__CHAINLOAD_H
#define PROTOS__CHAINLOAD_H

#include <stdnoreturn.h>
#include <stdbool.h>

noreturn void chainload(char *config, char *cmdline);

#if defined(UEFI)
noreturn void efi_load(char *config, char *cmdline, bool linux_protocol);
#endif

#endif
