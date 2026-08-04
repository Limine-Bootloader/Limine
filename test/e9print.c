#include <e9print.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if defined (_LIMINE_PROTO)
#include <flanterm.h>
extern struct flanterm_context *ft_ctx;
#endif

static const char CONVERSION_TABLE[] = "0123456789abcdef";

void putchar(char c) {
#if defined (__x86_64__) || defined (__i386__)
    __asm__ __volatile__ ("outb %0, %1" :: "a" (c), "Nd" (0xe9) : "memory");
#endif
#if defined (_LIMINE_PROTO)
    if (ft_ctx != NULL) {
        if (c == '\n') {
            flanterm_write(ft_ctx, "\r", 1);
        }
        flanterm_write(ft_ctx, &c, 1);
    }
#endif
}

void print(const char *msg) {
    for (size_t i = 0; msg[i]; i++) {
        putchar(msg[i]);
    }
}

void puts(const char *msg) {
    print(msg);
    putchar('\n');
}

static void printhex(uint64_t num, bool prefix) {
    int i;
    char buf[17];

    if (prefix) {
        print("0x");
    }

    if (!num) {
        putchar('0');
        return;
    }

    buf[16] = 0;

    for (i = 15; num; i--) {
        buf[i] = CONVERSION_TABLE[num % 16];
        num /= 16;
    }

    i++;
    print(&buf[i]);
}

static void printdec(uint64_t num) {
    int i;
    char buf[21] = {0};

    if (!num) {
        putchar('0');
        return;
    }

    for (i = 19; num; i--) {
        buf[i] = (num % 10) + 0x30;
        num = num / 10;
    }

    i++;
    print(buf + i);
}

void printf(const char *format, ...) {
    va_list argp;
    va_start(argp, format);

    while (*format != '\0') {
        if (*format != '%') {
            putchar(*format++);
            continue;
        }

        format++;
        bool prefix = false;
        if (*format == '#') {
            prefix = true;
            format++;
        }

        int width = 0;                      // 0 int, 1 long, 2 long long, 3 size_t
        if (*format == 'l') {
            width = format[1] == 'l' ? 2 : 1;
            format += width;
        } else if (*format == 'z') {
            width = 3;
            format++;
        }

        switch (*format) {
            case 'x':
                printhex(width == 0 ? va_arg(argp, unsigned)
                          : width == 1 ? va_arg(argp, unsigned long)
                          : width == 2 ? va_arg(argp, unsigned long long)
                                       : va_arg(argp, size_t), prefix);
                break;
            case 'u':
                printdec(width == 0 ? va_arg(argp, unsigned)
                          : width == 1 ? va_arg(argp, unsigned long)
                          : width == 2 ? va_arg(argp, unsigned long long)
                                       : va_arg(argp, size_t));
                break;
            case 'd':
                printdec(va_arg(argp, int));
                break;
            case 'p':
                printhex((uintptr_t)va_arg(argp, void *), true);
                break;
            case 's':
                print(va_arg(argp, char *));
                break;
            case 'c':
                putchar(va_arg(argp, int));
                break;
            default:
                putchar(*format);
                break;
        }
        format++;
    }

    va_end(argp);
}
