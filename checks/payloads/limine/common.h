#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);

void out_init(void);
void out_char(char c);
void out_str(const char *s);
void kprintf(const char *fmt, ...);
void vkprintf(const char *fmt, va_list args);
void out_escaped(const char *s, size_t max);
void out_hex(const void *data, size_t len);

uint64_t fnv1a(const void *data, size_t len);

// Report lines: "K key=value" for facts the harness cross-checks, "C PASS|FAIL
// name: detail" for checks made here against the protocol specification.
void fact(const char *key, const char *fmt, ...);
void fact_str(const char *key, const char *s);
bool check(bool cond, const char *name, const char *fmt, ...);
// Counted, but printed only on failure.
bool check_quiet(bool cond, const char *name, const char *fmt, ...);
void report_done(void);
__attribute__((noreturn)) void halt(void);

extern uint64_t hhdm_offset;
extern bool have_hhdm;

#endif
