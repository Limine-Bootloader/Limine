#include "common.h"

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = dest;
    const uint8_t *s = src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = s;
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)c;
    }
    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = dest;
    const uint8_t *s = src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != q[i]) {
            return p[i] < q[i] ? -1 : 1;
        }
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n] != 0) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a != 0 && *a == *b) {
        a++;
        b++;
    }
    return (uint8_t)*a - (uint8_t)*b;
}

uint64_t fnv1a(const void *data, size_t len) {
    const uint8_t *p = data;
    uint64_t h = 0xcbf29ce484222325;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3;
    }
    return h;
}

void out_str(const char *s) {
    while (*s != 0) {
        out_char(*s++);
    }
}

static void out_num(uint64_t v, unsigned base, int width, char pad) {
    char buf[24];
    int i = 0;
    do {
        buf[i++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v != 0);
    while (i < width) {
        buf[i++] = pad;
    }
    while (i > 0) {
        out_char(buf[--i]);
    }
}

void vkprintf(const char *fmt, va_list args) {
    for (; *fmt != 0; fmt++) {
        if (*fmt != '%') {
            out_char(*fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        int longs = 0;
        while (*fmt == 'l') {
            longs++;
            fmt++;
        }
        if (*fmt == 'z') {
            longs = 2;
            fmt++;
        }
        uint64_t v;
        switch (*fmt) {
            case 'd': {
                int64_t s = longs ? va_arg(args, int64_t) : va_arg(args, int);
                if (s < 0) {
                    out_char('-');
                    v = (uint64_t)0 - (uint64_t)s;
                } else {
                    v = s;
                }
                out_num(v, 10, width, pad);
                break;
            }
            case 'u':
                v = longs ? va_arg(args, uint64_t) : va_arg(args, unsigned);
                out_num(v, 10, width, pad);
                break;
            case 'x':
                v = longs ? va_arg(args, uint64_t) : va_arg(args, unsigned);
                out_num(v, 16, width, pad);
                break;
            case 'p':
                out_str("0x");
                out_num((uintptr_t)va_arg(args, void *), 16, width, pad);
                break;
            case 's': {
                const char *s = va_arg(args, const char *);
                out_str(s == NULL ? "(null)" : s);
                break;
            }
            case 'c':
                out_char((char)va_arg(args, int));
                break;
            case '%':
                out_char('%');
                break;
            default:
                out_char('?');
                break;
        }
    }
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf(fmt, args);
    va_end(args);
}

// Printable ASCII as is, everything else (and the escape character itself)
// as \xNN, so a value always fits on its report line.
void out_escaped(const char *s, size_t max) {
    for (size_t i = 0; i < max && s[i] != 0; i++) {
        uint8_t c = s[i];
        if (c >= 0x20 && c < 0x7f && c != '\\') {
            out_char(c);
        } else {
            out_char('\\');
            out_char('x');
            out_char("0123456789abcdef"[c >> 4]);
            out_char("0123456789abcdef"[c & 15]);
        }
    }
}

void out_hex(const void *data, size_t len) {
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        out_char("0123456789abcdef"[p[i] >> 4]);
        out_char("0123456789abcdef"[p[i] & 15]);
    }
}

void fact(const char *key, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kprintf("K %s=", key);
    vkprintf(fmt, args);
    out_char('\n');
    va_end(args);
}

void fact_str(const char *key, const char *s) {
    kprintf("K %s=", key);
    if (s == NULL) {
        out_str("(null)");
    } else {
        out_escaped(s, 8192);
    }
    out_char('\n');
}

static unsigned checks_run, checks_failed;

static bool vcheck(bool cond, bool quiet, const char *name, const char *fmt, va_list args) {
    checks_run++;
    if (cond) {
        if (!quiet) {
            kprintf("C PASS %s\n", name);
        }
        return true;
    }
    checks_failed++;
    kprintf("C FAIL %s: ", name);
    vkprintf(fmt, args);
    out_char('\n');
    return false;
}

bool check(bool cond, const char *name, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    bool r = vcheck(cond, false, name, fmt, args);
    va_end(args);
    return r;
}

bool check_quiet(bool cond, const char *name, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    bool r = vcheck(cond, true, name, fmt, args);
    va_end(args);
    return r;
}

void report_done(void) {
    kprintf("E DONE checks=%u failed=%u\n", checks_run, checks_failed);
}
