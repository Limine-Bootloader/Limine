#include <stdint.h>
#include <stddef.h>
#include <lib/misc.h>
#include <lib/print.h>

bool verbose = false;
bool quiet = false;
bool serial = false;
bool hash_mismatch_panic = false;
bool measured_boot = false;

uint8_t bcd_to_int(uint8_t val) {
    return (val & 0x0f) + ((val & 0xf0) >> 4) * 10;
}
uint8_t int_to_bcd(uint8_t val) {
    return (val % 10) | (val / 10) << 4;
}

int digit_to_int(char c) {
    if (c >= 'a' && c <= 'f') {
        return (c - 'a') + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return (c - 'A') + 10;
    }
    if (c >= '0' && c <= '9'){
        return c - '0';
    }

    return -1;
}

uint64_t strtoui(const char *s, const char **end, int base) {
    uint64_t n = 0;
    for (size_t i = 0; ; i++) {
        int d = digit_to_int(s[i]);
        if (d == -1 || d >= base) {
            if (end != NULL)
                *end = &s[i];
            break;
        }
        uint64_t mul_result = CHECKED_MUL(n, base, ({
            if (end != NULL)
                *end = &s[i];
            return UINT64_MAX;
        }));
        n = CHECKED_ADD(mul_result, d, ({
            if (end != NULL)
                *end = &s[i];
            return UINT64_MAX;
        }));
    }
    return n;
}

bool get_absolute_path(char *path_ptr, const char *path, const char *pwd, size_t size) {
    char *orig_ptr = path_ptr;
    char *end_ptr = path_ptr + size - 1;

    if (size == 0) return false;

    if (!*path) {
        size_t pwd_len = strlen(pwd);
        if (pwd_len >= size) return false;
        memcpy(path_ptr, pwd, pwd_len + 1);
        return true;
    }

    if (*path != '/') {
        size_t pwd_len = strlen(pwd);
        if (pwd_len >= size) return false;
        memcpy(path_ptr, pwd, pwd_len + 1);
        path_ptr += pwd_len;
    } else {
        *path_ptr = '/';
        path_ptr++;
        path++;
    }

    goto first_run;

    for (;;) {
        switch (*path) {
            case '/':
                path++;
first_run:
                if (*path == '/') continue;
                if ((!strncmp(path, ".\0", 2))
                ||  (!strncmp(path, "./\0", 3))) {
                    goto term;
                }
                if ((!strncmp(path, "..\0", 3))
                ||  (!strncmp(path, "../\0", 4))) {
                    while (path_ptr > orig_ptr && *path_ptr != '/') path_ptr--;
                    if (path_ptr == orig_ptr) path_ptr++;
                    goto term;
                }
                if (!strncmp(path, "../", 3)) {
                    while (path_ptr > orig_ptr && *path_ptr != '/') path_ptr--;
                    if (path_ptr == orig_ptr) path_ptr++;
                    path += 2;
                    *path_ptr = 0;
                    continue;
                }
                if (!strncmp(path, "./", 2)) {
                    path += 1;
                    continue;
                }
                if (path_ptr > orig_ptr && ((path_ptr - 1) != orig_ptr) && (*(path_ptr - 1) != '/')) {
                    if (path_ptr >= end_ptr) return false;
                    *path_ptr = '/';
                    path_ptr++;
                }
                continue;
            case '\0':
term:
                if (path_ptr > orig_ptr && (*(path_ptr - 1) == '/') && ((path_ptr - 1) != orig_ptr))
                    path_ptr--;
                *path_ptr = 0;
                return true;
            default:
                if (path_ptr >= end_ptr) return false;
                *path_ptr = *path;
                path++;
                path_ptr++;
                continue;
        }
    }
}
