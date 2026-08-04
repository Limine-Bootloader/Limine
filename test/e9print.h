#pragma once

#include <stdarg.h>
#include <stddef.h>

void putchar(char c);
void print(const char *msg);
void puts(const char *msg);
void printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
