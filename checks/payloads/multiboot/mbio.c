// Reporting for the Multiboot check kernels, on port 0xe9, in the
// "K key=value" format the harness parses.

#include "mbio.h"

void putc_(char c) {
    outb(0xe9, (uint8_t)c);
}

void puts_(const char *s) {
    while (*s != 0) {
        putc_(*s++);
    }
}

void hex(uint64_t v) {
    puts_("0x");
    bool started = false;
    for (int i = 60; i >= 0; i -= 4) {
        unsigned d = (v >> i) & 15;
        if (d != 0 || started || i == 0) {
            putc_("0123456789abcdef"[d]);
            started = true;
        }
    }
}

void fact(const char *key, uint64_t value) {
    puts_("K ");
    puts_(key);
    putc_('=');
    hex(value);
    putc_('\n');
}

void fact_str(const char *key, const char *value, size_t max) {
    puts_("K ");
    puts_(key);
    putc_('=');
    for (size_t i = 0; i < max && value[i] != 0; i++) {
        uint8_t c = value[i];
        if (c >= 0x20 && c < 0x7f && c != '\\') {
            putc_(c);
        } else {
            puts_("\\x");
            putc_("0123456789abcdef"[c >> 4]);
            putc_("0123456789abcdef"[c & 15]);
        }
    }
    putc_('\n');
}

void fact_hexdump(const char *key, const volatile void *data, size_t len) {
    const volatile uint8_t *p = data;
    puts_("K ");
    puts_(key);
    putc_('=');
    for (size_t i = 0; i < len; i++) {
        putc_("0123456789abcdef"[p[i] >> 4]);
        putc_("0123456789abcdef"[p[i] & 15]);
    }
    putc_('\n');
}

uint64_t fnv1a(const volatile void *data, uint64_t len) {
    const volatile uint8_t *p = data;
    uint64_t h = 0xcbf29ce484222325;
    for (uint64_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3;
    }
    return h;
}
