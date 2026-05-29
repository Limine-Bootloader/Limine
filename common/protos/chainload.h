#ifndef PROTOS__CHAINLOAD_H__
#define PROTOS__CHAINLOAD_H__

#include <stdnoreturn.h>

noreturn void chainload(char *config, char *cmdline);

noreturn void chainload_handover(char *config, char *cmdline);

#endif
