#if defined (BIOS)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/a20.h>
#include <sys/cpu.h>
#include <lib/real.h>

#define A20_KBC_TIMEOUT 50000

static bool a20_kbc_wait(uint8_t mask, uint8_t expected) {
    for (int i = 0; i < A20_KBC_TIMEOUT; i++) {
        if ((inb(0x64) & mask) == expected)
            return true;
    }
    return false;
}

bool a20_check(void) {
    bool ret = false;
    uint16_t orig = mminw(0x7dfe);

    mmoutw(0x7dfe, 0x1234);

    if (mminw(0x7dfe) != mminw(0x7dfe + 0x100000)) {
        ret = true;
        goto out;
    }

    mmoutw(0x7dfe, ~mminw(0x7dfe));

    if (mminw(0x7dfe) != mminw(0x7dfe + 0x100000)) {
        ret = true;
        goto out;
    }

out:
    mmoutw(0x7dfe, orig);
    return ret;
}

// Keyboard controller method code below taken from:
// https://wiki.osdev.org/A20_Line
// It predates the wiki's 2011 relicensing to CC0, but its author, Jhawthorn,
// opted all their contributions into CC0:
// https://web.archive.org/web/20150810185946/http://forum.osdev.org/viewtopic.php?f=8&t=23838&start=75#p196942

bool a20_enable(void) {
    if (a20_check())
        return true;

    // BIOS method
    struct rm_regs r = {0};
    r.eax = 0x2401;
    rm_int(0x15, &r, &r);

    if (a20_check())
        return true;

    // Keyboard controller method (with timeout for systems without KBC)
    if (!a20_kbc_wait(2, 0)) goto kbc_failed;
    outb(0x64, 0xad);
    if (!a20_kbc_wait(2, 0)) goto kbc_failed;
    outb(0x64, 0xd0);
    if (!a20_kbc_wait(1, 1)) goto kbc_failed;
    uint8_t b = inb(0x60);
    if (!a20_kbc_wait(2, 0)) goto kbc_failed;
    outb(0x64, 0xd1);
    if (!a20_kbc_wait(2, 0)) goto kbc_failed;
    outb(0x60, b | 2);
    if (!a20_kbc_wait(2, 0)) goto kbc_failed;
    outb(0x64, 0xae);
    if (!a20_kbc_wait(2, 0)) goto kbc_failed;

    if (a20_check())
        return true;

kbc_failed:

    // Fast A20 method
    b = inb(0x92);
    if ((b & 0x02) == 0) {
        b &= ~0x01;
        b |= 0x02;
        outb(0x92, b);
    }

    if (a20_check())
        return true;

    return false;
}

#endif
