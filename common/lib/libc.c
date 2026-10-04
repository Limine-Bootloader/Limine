#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <mm/pmm.h>

// No base admits UINT_MAX, which stands for anything but a digit or letter.
static unsigned int digit_value(int c) {
    if (isdigit(c)) {
        return c - '0';
    }
    if (isalpha(c)) {
        return tolower(c) - 'a' + 10;
    }
    return UINT_MAX;
}

unsigned long strtoul(const char *str, char **end, int base) {
    if (base != 0 && (base < 2 || base > 36)) {
        if (end != NULL) {
            *end = (char *)str;
        }
        return 0;
    }

    const char *p = str;
    bool negative = false;

    while (isspace((unsigned char)*p)) {
        p++;
    }

    if (*p == '+' || *p == '-') {
        negative = *p == '-';
        p++;
    }

    // Without a hexadecimal digit after it, 0x is no prefix and the subject
    // sequence ends at the 0 (C11 7.22.1.4, 6.4.4.1).
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')
     && digit_value((unsigned char)p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }

    const char *digits = p;
    unsigned long radix = base;
    unsigned long value = 0;
    bool overflow = false;
    unsigned int digit;

    while ((digit = digit_value((unsigned char)*p)) < radix) {
        if (__builtin_mul_overflow(value, radix, &value)
         || __builtin_add_overflow(value, digit, &value)) {
            overflow = true;
        }
        p++;
    }

    if (end != NULL) {
        // Without digits nothing is converted, not even white space or a sign.
        *end = (char *)(p == digits ? str : p);
    }

    if (overflow) {
        return ULONG_MAX;
    }

    return negative ? -value : value;
}

size_t strnlen(const char *str, size_t maxlen) {
    size_t len;

    for (len = 0; len < maxlen && str[len]; len++);

    return len;
}

void *memchr(const void *ptr, int ch, size_t n) {
    uint8_t *p = (uint8_t *)ptr;

    for (size_t i = 0; i < n; i++) {
        if (p[i] == ch) {
            return (void *)ptr + i;
        }
    }

    return NULL;
}

char *strchr(const char *str, int ch) {
    for (size_t i = 0; ; i++) {
        if (str[i] == (char)ch) {
            return (char *)str + i;
        }
        if (str[i] == '\0') {
            return NULL;
        }
    }
}

char *strrchr(const char *str, int ch) {
    char *p = NULL;

    for (size_t i = 0; ; i++) {
        if (str[i] == (char)ch) {
            p = (char *)str + i;
        }
        if (str[i] == '\0') {
            break;
        }
    }

    return p;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *buf = ext_mem_alloc(len);
    memcpy(buf, s, len);
    return buf;
}
