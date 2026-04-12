#ifndef LIB__MISC_H__
#define LIB__MISC_H__

#include <stdbool.h>

__attribute__((noreturn))
void panic(bool nostack, const char *fmt, ...);

#endif
