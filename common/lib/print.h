#ifndef LIB__PRINT_H
#define LIB__PRINT_H

#include <stdarg.h>
#include <stdbool.h>

extern bool verbose;

void print(const char *fmt, ...);
void vprint(const char *fmt, va_list args);

#define printv(FMT, ...) do { \
    if (verbose) print(FMT, ##__VA_ARGS__); \
} while (0)

#endif
