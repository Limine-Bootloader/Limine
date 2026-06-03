#include <stddef.h>
#include <stdint.h>
#include <lib/libc.h>
#include <stdbool.h>
#include <lib/misc.h>

bool isprint(int c) {
    return c >= ' ' && c <= '~';
}

bool isspace(int c) {
    return (c >= '\t' && c <= 0xD) || c == ' ';
}

bool isalpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isdigit(int c) {
    return c >= '0' && c <= '9';
}

int toupper(int c) {
    if (c >= 'a' && c <= 'z') {
        return c - 0x20;
    }
    return c;
}

int tolower(int c) {
    if (c >= 'A' && c <= 'Z') {
        return c + 0x20;
    }
    return c;
}

int abs(int i) {
    return i < 0 ? -i : i;
}

char *strcpy(char *dest, const char *src) {
    size_t i;

    for (i = 0; src[i]; i++)
        dest[i] = src[i];

    dest[i] = 0;

    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;

    for (i = 0; i < n && src[i]; i++)
        dest[i] = src[i];
    for ( ; i < n; i++)
        dest[i] = 0;

    return dest;
}

int strcmp(const char *s1, const char *s2) {
    for (size_t i = 0; ; i++) {
        unsigned char c1 = ((unsigned char *)s1)[i], c2 = ((unsigned char *)s2)[i];
        if (c1 != c2) {
            return c1 < c2 ? -1 : 1;
        }
        if (c1 == 0) {
            return 0;
        }
    }
}

int strcasecmp(const char *s1, const char *s2) {
    for (size_t i = 0; ; i++) {
        unsigned char c1 = ((unsigned char *)s1)[i], c2 = ((unsigned char *)s2)[i];
        if (tolower(c1) != tolower(c2)) {
            return tolower(c1) < tolower(c2) ? -1 : 1;
        }
        if (c1 == 0) {
            return 0;
        }
    }
}

int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c1 = ((unsigned char *)s1)[i], c2 = ((unsigned char *)s2)[i];
        if (c1 != c2) {
            return c1 < c2 ? -1 : 1;
        }
        if (c1 == 0) {
            return 0;
        }
    }

    return 0;
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c1 = ((unsigned char *)s1)[i], c2 = ((unsigned char *)s2)[i];
        if (tolower(c1) != tolower(c2)) {
            return tolower(c1) < tolower(c2) ? -1 : 1;
        }
        if (c1 == 0) {
            return 0;
        }
    }

    return 0;
}

size_t strlen(const char *str) {
    size_t len;

    for (len = 0; str[len]; len++);

    return len;
}

int inet_pton(const char *src, void *dst) {
    uint8_t array[4];
    const char *current = src;

    for (int i = 0; i < 4; i++) {
        const char *newcur;
        uint64_t value = strtoui(current, &newcur, 10);
        if (current == newcur)
            return -1;
        current = newcur;
        if (i < 3) {
            // Expect '.' delimiter between octets
            if (*current != '.')
                return -1;
            current++;
        } else {
            // After last octet, string must end
            if (*current != 0)
                return -1;
        }
        if (value > 255)
            return -1;
        array[i] = value;
    }
    memcpy(dst, array, 4);
    return 0;
}
