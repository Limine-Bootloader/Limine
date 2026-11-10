#if defined(__x86_64__) || defined(__i386__)

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/pic.h>
#include <sys/cpu.h>
#include <lib/misc.h>

void pic_eoi(int irq) {
    if (irq >= 8) {
        outb(0xa0, 0x20);
    }

    outb(0x20, 0x20);
}

// Flush all potentially pending IRQs
void pic_flush(void) {
    for (int i = 0; i < 16; i++)
        pic_eoi(i);
}

void pic_set_mask(int line, bool status) {
    uint16_t port;
    uint8_t value;

    if (line < 8) {
        port = 0x21;
    } else {
        port = 0xa1;
        line -= 8;
    }

    if (!status)
        value = inb(port) & ~((uint8_t)1 << line);
    else
        value = inb(port) | ((uint8_t)1 << line);

    outb(port, value);
}

void pic_mask_all(void) {
    outb(0xa1, 0xff);
    outb(0x21, 0xff);
}

#endif
