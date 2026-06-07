#include <stddef.h>
#include <stdint.h>
#include <lib/time.h>
#if defined (BIOS)
#  include <lib/real.h>
#elif defined (UEFI)
#  include <efi.h>
#endif
#include <lib/misc.h>

// Julian date calculation from https://en.wikipedia.org/wiki/Julian_day
static int get_jdn(int days, int months, int years) {
    return (1461 * (years + 4800 + (months - 14)/12))/4 + (367 *
           (months - 2 - 12 * ((months - 14)/12)))/12 -
           (3 * ((years + 4900 + (months - 14)/12)/100))/4
           + days - 32075;
}

static uint64_t get_unix_epoch(uint8_t seconds, uint8_t minutes, uint8_t  hours,
                               uint8_t days,    uint8_t months,  uint16_t years) {
    uint64_t jdn_current = get_jdn(days, months, years);
    uint64_t jdn_1970    = get_jdn(1, 1, 1970);

    uint64_t jdn_diff = jdn_current - jdn_1970;

    return (jdn_diff * (60 * 60 * 24)) + hours * 3600 + minutes * 60 + seconds;
}

#if defined (BIOS)
uint64_t time(void) {
    struct rm_regs r;

again:
    r = (struct rm_regs){0};
    r.eax = 0x0400;
    rm_int(0x1a, &r, &r);
    if (r.eflags & EFLAGS_CF) {
        return 0;
    }

    uint8_t  day    = bcd_to_int( r.edx & 0x00ff);
    uint8_t  month  = bcd_to_int((r.edx & 0xff00) >> 8);
    uint16_t year   = bcd_to_int( r.ecx & 0x00ff) +
    /* century */     bcd_to_int((r.ecx & 0xff00) >> 8) * 100;

    r = (struct rm_regs){0};
    r.eax = 0x0200;
    rm_int(0x1a, &r, &r);
    if (r.eflags & EFLAGS_CF) {
        return 0;
    }

    uint8_t second  = bcd_to_int((r.edx & 0xff00) >> 8);
    uint8_t minute  = bcd_to_int( r.ecx & 0x00ff);
    uint8_t hour    = bcd_to_int((r.ecx & 0xff00) >> 8);

    // Check RTC day wraparound
    r = (struct rm_regs){0};
    r.eax = 0x0400;
    rm_int(0x1a, &r, &r);
    if (r.eflags & EFLAGS_CF) {
        return 0;
    }
    if (bcd_to_int(r.edx & 0x00ff) != day) {
        goto again;
    }

    return get_unix_epoch(second, minute, hour, day, month, year);
}
#endif

#if defined (UEFI)
uint64_t time(void) {
    EFI_TIME time;
    EFI_STATUS status = gRT->GetTime(&time, NULL);

    if (status != 0) {
        return 0;
    }

    return get_unix_epoch(time.Second, time.Minute, time.Hour,
                          time.Day, time.Month, time.Year);
}
#endif
