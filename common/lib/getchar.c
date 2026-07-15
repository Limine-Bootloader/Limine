#include <stdint.h>
#include <stddef.h>
#include <lib/getchar.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/term.h>
#include <lib/print.h>
#include <menu.h>
#if defined (BIOS)
#  include <lib/real.h>
#elif defined (UEFI)
#  include <efi.h>
#endif
#include <drivers/mouse.h>
#include <drivers/serial.h>
#include <sys/cpu.h>

enum {
    MOUSE_MODE_OFF,
    MOUSE_MODE_NO_MOVES,
    MOUSE_MODE_FULL,
};

static const char qwerty_to_dvorak[128] = {
    ['q']='\'', ['w']=',', ['e']='.', ['r']='p', ['t']='y',
    ['y']='f', ['u']='g', ['i']='c', ['o']='r', ['p']='l',
    ['[']='/', [']']='=', ['\\']='\\', ['a']='a', ['s']='o',
    ['d']='e', ['f']='u', ['g']='i', ['h']='d', ['j']='h',
    ['k']='t', ['l']='n', [';']='s', ['\'']='-', ['z']=';', ['x']='q',
    ['c']='j', ['v']='k', ['b']='x', ['n']='b', ['m']='m',
    [',']='w', ['.']='v', ['/']='z',

    ['Q']='\"', ['W']='<', ['E']='>', ['R']='P', ['T']='Y',
    ['Y']='F', ['U']='G', ['I']='C', ['O']='R', ['P']='L',
    ['{']='?', ['}']='+', ['|']='|', ['A']='A', ['S']='O',
    ['D']='E', ['F']='U', ['G']='I', ['H']='D', ['J']='H',
    ['K']='T', ['L']='N', [':']='S', ['"']='_', ['Z']=':', ['X']='Q',
    ['C']='J', ['V']='K', ['B']='X', ['N']='B', ['M']='M',
    ['<']='W', ['>']='V', ['?']='Z',

    ['`']='`', ['1']='1', ['2']='2', ['3']='3', ['4']='4',
    ['5']='5', ['6']='6', ['7']='7', ['8']='8', ['9']='9',
    ['0']='0', ['-']='[', ['=']=']',

    ['~']='~', ['!']='!', ['@']='@', ['#']='#', ['$']='$',
    ['%']='%', ['^']='^', ['&']='&', ['*']='*', ['(']='(',
    [')']=')', ['_']='{', ['+']='}',
};

static const unsigned char qwerty_to_azerty[128] = {
    ['q']='a', ['w']='z', ['e']='e', ['r']='r', ['t']='t',
    ['y']='y', ['u']='u', ['i']='i', ['o']='o', ['p']='p',
    ['[']='^', [']']='$', ['\\']='\\', ['a']='q', ['s']='s',
    ['d']='d', ['f']='f', ['g']='g', ['h']='h', ['j']='j',
    ['k']='k', ['l']='l', [';']='m', ['\'']='\xF9', ['z']='w', ['x']='x',
    ['c']='c', ['v']='v', ['b']='b', ['n']='n', ['m']=',',
    [',']=';', ['.']=':', ['/']='!',

    ['Q']='A', ['W']='Z', ['E']='E', ['R']='R', ['T']='T',
    ['Y']='Y', ['U']='U', ['I']='I', ['O']='O', ['P']='P',
    ['|']='|', ['A']='Q', ['S']='S',
    ['D']='D', ['F']='F', ['G']='G', ['H']='H', ['J']='J',
    ['K']='K', ['L']='L', [':']='M', ['"']='%', ['Z']='W', ['X']='X',
    ['C']='C', ['V']='V', ['B']='B', ['N']='N', ['M']='?',
    ['<']='.', ['>']='/', ['?']='\xA7',

    ['`']='\xB2', ['1']='&', ['2']='\xE9', ['3']='\"', ['4']='\'',
    ['5']='(', ['6']='-', ['7']='\xE8', ['8']='_', ['9']='\xE7',
    ['0']='\xE0', ['-']=')', ['=']='=',

    ['!']='1', ['@']='2', ['#']='3', ['$']='4', ['%']='5',
    ['^']='6', ['&']='7', ['*']='8', ['(']='9', [')']='0',
    ['_']='\xB0', ['+']='+',
};

int getchar(void) {
    for (;;) {
        int ret = pit_sleep_and_quit_on_keypress(65535);
        if (ret != 0) {
            return ret;
        }
    }
}

int getchar_internal(uint8_t scancode, uint8_t ascii, uint32_t shift_state) {
    switch (scancode) {
#if defined (BIOS)
        case 0x44:
            return GETCHAR_F10;
        case 0x4b:
            return GETCHAR_CURSOR_LEFT;
        case 0x4d:
            return GETCHAR_CURSOR_RIGHT;
        case 0x48:
            return GETCHAR_CURSOR_UP;
        case 0x50:
            return GETCHAR_CURSOR_DOWN;
        case 0x53:
            return GETCHAR_DELETE;
        case 0x4f:
            return GETCHAR_END;
        case 0x47:
            return GETCHAR_HOME;
        case 0x49:
            return GETCHAR_PGUP;
        case 0x51:
            return GETCHAR_PGDOWN;
        case 0x01:
            return GETCHAR_ESCAPE;
#elif defined (UEFI)
        case SCAN_F10:
            return GETCHAR_F10;
        case SCAN_LEFT:
            return GETCHAR_CURSOR_LEFT;
        case SCAN_RIGHT:
            return GETCHAR_CURSOR_RIGHT;
        case SCAN_UP:
            return GETCHAR_CURSOR_UP;
        case SCAN_DOWN:
            return GETCHAR_CURSOR_DOWN;
        case SCAN_DELETE:
            return GETCHAR_DELETE;
        case SCAN_END:
            return GETCHAR_END;
        case SCAN_HOME:
            return GETCHAR_HOME;
        case SCAN_PAGE_UP:
            return GETCHAR_PGUP;
        case SCAN_PAGE_DOWN:
            return GETCHAR_PGDOWN;
        case SCAN_ESC:
            return GETCHAR_ESCAPE;
#endif
    }
    switch (ascii) {
        case '\n':
        case '\r':
            return '\n';
        case '\b':
            return '\b';
        case '\t':
            return '\t';
    }

    // Ctrl+<letter> arrives either as the letter plus a Ctrl shift state, or
    // (commonly) as the bare control code 0x01-0x1a; handle both.
    uint8_t ctrl_char = 0;
    if (ascii >= 0x01 && ascii <= 0x1a) {
        ctrl_char = ascii | 0x60;
    } else if (shift_state & (GETCHAR_LCTRL | GETCHAR_RCTRL)) {
        ctrl_char = ascii;
    }
    switch (ctrl_char) {
    case 'a': return GETCHAR_HOME;
    case 'e': return GETCHAR_END;
    case 'p': return GETCHAR_CURSOR_UP;
    case 'n': return GETCHAR_CURSOR_DOWN;
    case 'b': return GETCHAR_CURSOR_LEFT;
    case 'f': return GETCHAR_CURSOR_RIGHT;
    default: break;
    }

    // Guard against non-printable values
    if (ascii < 0x20 || ascii > 0x7e) {
        return -1;
    }

    if (current_keyboard_layout == KEYBOARD_LAYOUT_DVORAK && qwerty_to_dvorak[(uint8_t)ascii] != 0) {
        return qwerty_to_dvorak[(uint8_t)ascii];
    } else if (current_keyboard_layout == KEYBOARD_LAYOUT_AZERTY && qwerty_to_azerty[(uint8_t)ascii] != 0) {
        return qwerty_to_azerty[(uint8_t)ascii];
    }

    return ascii;
}

#if defined (BIOS)
int _pit_sleep_and_quit_on_keypress(uint32_t ticks, uint32_t aux_poll);

// XXX: sync with lib/sleep.asm_bios_ia32.
#define PIT_SLEEP_AUX_BREAK (-100)

// BDA tick counter at 0x46c wraps at midnight.
#define BDA_TICKS_PER_DAY 0x1800b0

static int input_sequence(void) {
    int val = 0;

    for (;;) {
        int ret = -1;
        size_t retries = 0;

        while (ret == -1 && retries < 1000000) {
            ret = serial_in();
            retries++;
        }
        if (ret == -1) {
            return 0;
        }

        switch (ret) {
            case 'A':
                return GETCHAR_CURSOR_UP;
            case 'B':
                return GETCHAR_CURSOR_DOWN;
            case 'C':
                return GETCHAR_CURSOR_RIGHT;
            case 'D':
                return GETCHAR_CURSOR_LEFT;
            case 'F':
                return GETCHAR_END;
            case 'H':
                return GETCHAR_HOME;
        }

        if (ret > '9' || ret < '0') {
            break;
        }

        val *= 10;
        val += ret - '0';
    }

    switch (val) {
        case 3:
            return GETCHAR_DELETE;
        case 5:
            return GETCHAR_PGUP;
        case 6:
            return GETCHAR_PGDOWN;
        case 21:
            return GETCHAR_F10;
    }

    return 0;
}

static int serial_input(void) {
    int ret = serial_in();

    if (ret == -1) {
        return 0;
    }

again:
    switch (ret) {
        case '\r':
            return '\n';
        case 0x1b:
            stall(10);
            ret = serial_in();
            if (ret == -1) {
                return GETCHAR_ESCAPE;
            }
            if (ret == '[') {
                return input_sequence();
            }
            goto again;
        case 0x7f:
            return '\b';
    }

    return ret;
}

static int sleep_ms_core(uint64_t milliseconds, int mouse_mode) {
    uint64_t ticks64 = milliseconds > (UINT64_MAX - 999) / 18
                     ? UINT64_MAX
                     : (milliseconds * 18 + 999) / 1000;
    uint32_t ticks = ticks64 > UINT32_MAX ? UINT32_MAX : ticks64;

    if (ticks == 0) {
        return 0;
    }

    // Hand over mouse state accumulated while nobody was listening (e.g. a
    // pointer position preserved across a menu re-entry) before blocking.
    if (mouse_mode == MOUSE_MODE_FULL && mouse_state_pending()) {
        return GETCHAR_MOUSE;
    }

    // When the mouse is active its packets are always consumed, even for
    // keyboard-only waits.
    bool aux_poll = mouse_present();

    if (!serial && !aux_poll) {
        return _pit_sleep_and_quit_on_keypress(ticks, 0);
    }

    uint32_t start = mmind(0x46c);
    uint32_t elapsed = 0;

    for (;;) {
        uint32_t remaining = ticks - elapsed;
        int ret = _pit_sleep_and_quit_on_keypress(serial && remaining > 1 ? 1 : remaining,
                                                  aux_poll);

        if (ret == PIT_SLEEP_AUX_BREAK) {
            int ev = mouse_process_pending();
            if (mouse_mode != MOUSE_MODE_OFF && ev != 0) {
                if (ev & (MOUSE_EVENT_BUTTON | MOUSE_EVENT_WHEEL)
                 || mouse_mode == MOUSE_MODE_FULL) {
                    return GETCHAR_MOUSE;
                }
                mouse_render_pointer();
            }
        } else if (ret != 0) {
            return ret;
        }

        if (serial) {
            ret = serial_input();
            if (ret != 0) {
                return ret;
            }
        }

        uint32_t now = mmind(0x46c);
        elapsed = now >= start ? now - start
                : now + (uint32_t)(BDA_TICKS_PER_DAY - start);
        if (elapsed >= ticks) {
            return 0;
        }
    }
}

int pit_sleep_ms_and_quit_on_keypress(uint64_t milliseconds) {
    return sleep_ms_core(milliseconds, MOUSE_MODE_OFF);
}

int pit_sleep_ms_and_quit_on_input(uint64_t milliseconds, bool deliver_mouse_moves) {
    return sleep_ms_core(milliseconds,
                         deliver_mouse_moves ? MOUSE_MODE_FULL : MOUSE_MODE_NO_MOVES);
}

int pit_sleep_and_quit_on_keypress(int seconds) {
    return pit_sleep_ms_and_quit_on_keypress((uint64_t)seconds * 1000);
}
#endif

#if defined (UEFI)
static int input_sequence(bool ext,
                   EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *exproto,
                   EFI_SIMPLE_TEXT_IN_PROTOCOL *sproto) {
    EFI_STATUS status;
    EFI_KEY_DATA kd;

    int val = 0;

    for (;;) {
        if (ext == false) {
            status = sproto->ReadKeyStroke(sproto, &kd.Key);
        } else {
            status = exproto->ReadKeyStrokeEx(exproto, &kd);
        }

        if (status != EFI_SUCCESS) {
            return 0;
        }

        switch (kd.Key.UnicodeChar) {
            case 'A':
                return GETCHAR_CURSOR_UP;
            case 'B':
                return GETCHAR_CURSOR_DOWN;
            case 'C':
                return GETCHAR_CURSOR_RIGHT;
            case 'D':
                return GETCHAR_CURSOR_LEFT;
            case 'F':
                return GETCHAR_END;
            case 'H':
                return GETCHAR_HOME;
        }

        if (kd.Key.UnicodeChar > '9' || kd.Key.UnicodeChar < '0') {
            break;
        }

        val *= 10;
        val += kd.Key.UnicodeChar - '0';
    }

    switch (val) {
        case 3:
            return GETCHAR_DELETE;
        case 5:
            return GETCHAR_PGUP;
        case 6:
            return GETCHAR_PGDOWN;
        case 21:
            return GETCHAR_F10;
    }

    return 0;
}

static int sleep_ms_core(uint64_t milliseconds, int mouse_mode) {
    EFI_KEY_DATA kd;

    UINTN which;

    EFI_EVENT events[18];

    size_t pointer_count = 0;

    // Hand over mouse state accumulated while nobody was listening (e.g. a
    // pointer position preserved across a menu re-entry) before blocking.
    if (mouse_mode == MOUSE_MODE_FULL && mouse_state_pending()) {
        return GETCHAR_MOUSE;
    }

    EFI_GUID exproto_guid = EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
    EFI_GUID sproto_guid = EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *exproto = NULL;
    EFI_SIMPLE_TEXT_IN_PROTOCOL *sproto = NULL;

    bool use_sproto = false;

    if (gBS->HandleProtocol(gST->ConsoleInHandle, &exproto_guid, (void **)&exproto) != EFI_SUCCESS) {
        if (gBS->HandleProtocol(gST->ConsoleInHandle, &sproto_guid, (void **)&sproto) != EFI_SUCCESS) {
            if (gST->ConIn != NULL) {
                sproto = gST->ConIn;
            } else {
                panic(false, "Your input device doesn't have an input protocol!");
            }
        }

        events[0] = sproto->WaitForKey;

        use_sproto = true;
    } else {
        events[0] = exproto->WaitForKeyEx;
    }

    if (mouse_mode != MOUSE_MODE_OFF) {
        pointer_count = mouse_get_efi_events(&events[2], 16);
    }

restart:
    gBS->CreateEvent(EVT_TIMER, TPL_CALLBACK, NULL, NULL, &events[1]);

    gBS->SetTimer(events[1], TimerRelative,
                  milliseconds > UINT64_MAX / 10000 ? UINT64_MAX : milliseconds * 10000);

again:
    memset(&kd, 0, sizeof(EFI_KEY_DATA));

    gBS->WaitForEvent(2 + pointer_count, events, &which);

    if (which == 1) {
        gBS->CloseEvent(events[1]);
        return 0;
    }

    if (which >= 2) {
        int ev = mouse_handle_efi_event(which - 2);
        if (ev != 0) {
            if (ev & (MOUSE_EVENT_BUTTON | MOUSE_EVENT_WHEEL)
             || mouse_mode == MOUSE_MODE_FULL) {
                gBS->CloseEvent(events[1]);
                return GETCHAR_MOUSE;
            }
            mouse_render_pointer();
        }
        goto again;
    }

    EFI_STATUS status;
    if (use_sproto) {
        status = sproto->ReadKeyStroke(sproto, &kd.Key);
    } else {
        status = exproto->ReadKeyStrokeEx(exproto, &kd);
    }

    if (status != EFI_SUCCESS) {
        goto again;
    }

    if ((kd.KeyState.KeyShiftState & EFI_SHIFT_STATE_VALID) == 0) {
        kd.KeyState.KeyShiftState = 0;
    }

    if (serial == true && kd.Key.ScanCode == 0x08) {
        gBS->CloseEvent(events[1]);
        return '\b';
    }

    if (kd.Key.ScanCode == SCAN_ESC) {
        gBS->CloseEvent(events[1]);

        gBS->CreateEvent(EVT_TIMER, TPL_CALLBACK, NULL, NULL, &events[1]);

        gBS->SetTimer(events[1], TimerRelative, 100000);

        gBS->WaitForEvent(2, events, &which);

        if (which == 1) {
            gBS->CloseEvent(events[1]);
            return GETCHAR_ESCAPE;
        }

        if (use_sproto) {
            status = sproto->ReadKeyStroke(sproto, &kd.Key);
        } else {
            status = exproto->ReadKeyStrokeEx(exproto, &kd);
        }

        gBS->CloseEvent(events[1]);

        if (status != EFI_SUCCESS) {
            goto restart;
        }

        if (kd.Key.UnicodeChar == '[') {
            return input_sequence(!use_sproto, exproto, sproto);
        }

        goto restart;
    }

    int ret = getchar_internal(kd.Key.ScanCode, kd.Key.UnicodeChar,
                               kd.KeyState.KeyShiftState);

    if (ret == -1) {
        goto again;
    }

    gBS->CloseEvent(events[1]);
    return ret;
}

int pit_sleep_ms_and_quit_on_keypress(uint64_t milliseconds) {
    return sleep_ms_core(milliseconds, MOUSE_MODE_OFF);
}

int pit_sleep_ms_and_quit_on_input(uint64_t milliseconds, bool deliver_mouse_moves) {
    return sleep_ms_core(milliseconds,
                         deliver_mouse_moves ? MOUSE_MODE_FULL : MOUSE_MODE_NO_MOVES);
}

int pit_sleep_and_quit_on_keypress(int seconds) {
    return pit_sleep_ms_and_quit_on_keypress((uint64_t)seconds * 1000);
}
#endif
