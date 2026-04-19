#ifndef LIBC_COMPAT__STRING_H__
#define LIBC_COMPAT__STRING_H__

#include <stddef.h>

void *memset(void *, int, size_t);
void *memcpy(void *restrict, const void *restrict, size_t);
int memcmp(const void *, const void *, size_t);
void *memmove(void *, const void *, size_t);

#define memset __builtin_memset
#define memcpy __builtin_memcpy
#define memcmp __builtin_memcmp
#define memmove __builtin_memmove

void *memchr(const void *, int, size_t);

char *strcpy(char *, const char *);
char *strncpy(char *, const char *, size_t);
char *strchr(const char *, int);
char *strrchr(const char *, int);
size_t strlen(const char *);
size_t strnlen(const char *, size_t);
int strcmp(const char *, const char *);
int strcasecmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
int strncasecmp(const char *, const char *, size_t);

#endif
