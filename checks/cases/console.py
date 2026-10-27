import re
import time

from harness.api import check
from harness.ports import ALL_PORTS, UEFI_PORTS
from cases._common import entry, limine_version, screen_text
from cases.menu import BRANDING_ARCH, KERNEL, boot_menu, booted_cmdline, nothing_booted, tree_menu

# Where Limine's interface goes and how much it says: serial, graphics, quiet,
# verbose, terse and serial_baudrate, as CONFIG.md describes them. [intent]
# marks what only Limine's code says, such as the wording of its messages.

DOWN = b'\x1b[B'


def type_serial(m, *keys):
    for key in keys:
        m.stream('serial').write(key)
        time.sleep(0.3)


@check(ports=ALL_PORTS, tags=['console'])
def console_serial_menu(t):
    """serial: yes puts the menu on the serial line, drawn in ASCII [intent],
    and takes keys typed there."""
    m = boot_menu(t, tree_menu('timeout: no\nserial: yes\n'))
    # The comment can follow the rest onto the line.
    t.wait_serial_text(m, 'the first comment')
    text = screen_text(t.wait_serial_text(m, 'ARROWS'))
    branding = f'Limine {limine_version(t.env)} ({BRANDING_ARCH[t.port.name]})'
    t.check(branding in text, f'[intent] branding on the serial line:\n{text}')
    for line in ('First', '[-] Dir', ' |--> Inner One', ' `--> Inner Two', '[+] Collapsed', 'Last',
                 'the first comment'):
        t.check(line in text, f'{line!r} on the serial line')
    type_serial(m, DOWN, DOWN)
    t.wait_serial_text(m, 'an inner comment')
    type_serial(m, b'\r')
    t.expect_eq(booted_cmdline(t, m), 'inner one', 'keys typed on the serial line chose and booted Inner One')


@check(ports=ALL_PORTS, tags=['console'])
def console_graphics_no(t):
    """graphics: no draws the menu in text mode: VGA text mode on BIOS, and
    on UEFI, which has no text mode of its own, the firmware's console
    [intent]."""
    uefi = t.port.is_uefi
    m = boot_menu(t, tree_menu('timeout: no\ngraphics: no\n'))
    scr = t.wait_text(m, 'ARROWS', firmware=uefi)
    if not uefi:
        img = m.screendump()
        t.expect_eq((img.width, img.height), (720, 400), 'the adapter shows 80x25 text')
    text = screen_text(scr)
    for name in ('First', '[-] Dir', 'Inner One', 'Last'):
        t.check(name in text, f'entry {name!r} listed')
    m.press('down', 'down')
    t.wait_text(m, 'an inner comment', firmware=uefi)
    m.press('ret')
    t.expect_eq(booted_cmdline(t, m), 'inner one', 'booted from the text mode menu')


def limine_shown(t, m):
    """Whether any of Limine's interface is on screen, in its own terminal
    or, on UEFI, through the firmware's console."""
    texts = [screen_text(t.screen(m, 'poll'))]
    if t.port.is_uefi:
        texts.append(screen_text(t.firmware_screen(m, 'poll')))
    return any('ARROWS' in text or 'Limine ' in text for text in texts)


@check(ports=ALL_PORTS, tags=['console'])
def console_quiet(t):
    """quiet: yes shows nothing while the timeout runs and then boots the
    default entry; a key pressed during the timeout shows the menu and stops
    it; with autoboot off the menu is shown regardless."""
    m = boot_menu(t, tree_menu('timeout: 3\nquiet: yes\n'))
    shown = False
    while 'E DONE' not in m.stream(t.kernel_channel).text() and not m.exited():
        shown = shown or limine_shown(t, m)
        time.sleep(0.3)
    t.check(not shown, 'nothing of Limine shown while the timeout ran')
    t.expect_eq(booted_cmdline(t, m), 'first', 'the default entry booted when the timeout ran out')
    m.stop()
    # Pressed until it lands, as however long the firmware takes, one press
    # comes within the timeout; Right does nothing in the menu itself.
    m = boot_menu(t, tree_menu('timeout: 6\nquiet: yes\n'))
    deadline = time.monotonic() + 120 * t.port.timeout_scale
    while not limine_shown(t, m):
        t.require('B LIMINE-TEST' not in m.stream(t.kernel_channel).text() and time.monotonic() < deadline,
                  'a key pressed during the timeout stopped it')
        m.press('right')
        time.sleep(0.5)
    scr = t.wait_text(m, 'ARROWS')
    t.check('the first comment' in screen_text(scr), 'the menu shown, the default entry selected')
    t.check(nothing_booted(t, m, 8), 'the timeout stopped')
    m.stop()
    m = boot_menu(t, tree_menu('timeout: no\nquiet: yes\n'))
    t.wait_text(m, 'ARROWS')


def boot_messages(t, globals_):
    """Boot an entry with a module, serial: yes and globals_; return the text
    Limine wrote to the serial line, and the executable's report."""
    config = 'timeout: 0\nserial: yes\n' + globals_ + entry(
        'Test', path=KERNEL, cmdline='messages', modules=[('boot():/boot/mod1.bin', 'one')])
    m = boot_menu(t, config, files={'/boot/mod1.bin': b'module contents\n'})
    report = t.kernel_report(m)
    text = t.serial_plain(m)
    # Where the executable writes to the serial line too, what it wrote.
    text = text.split('B LIMINE-TEST', 1)[0]
    m.stop()
    return text, report


LOADING = ('limine: Loading executable `boot():/boot/kernel.elf`...', 'limine: Loading module `boot():/boot/mod1.bin`...')


@check(ports=ALL_PORTS, tags=['console'])
def console_message_levels(t):
    """Loading messages are printed by default and not with terse: yes;
    verbose: yes adds the executable's load addresses; quiet: yes prints
    nothing at all [intent]."""
    text, report = boot_messages(t, '')
    for line in LOADING:
        t.check(line in text, f'[intent] {line!r} printed by default:\n{text}')
    t.check('limine: Physical base:' not in text, 'nothing verbose by default')
    text, report = boot_messages(t, 'terse: yes\n')
    for line in LOADING:
        t.check(line not in text, f'[spec] terse: {line!r} not printed')
    t.expect_eq(report['cmdline'], 'messages', 'terse: booted')
    text, report = boot_messages(t, 'verbose: yes\n')
    for label, key in (('Physical base', 'physical_base'), ('Virtual base', 'virtual_base')):
        found = re.search(rf'limine: {label}: +0x([0-9a-f]+)', text)
        t.check(found is not None and found.group(1) == report[f'executable_address.{key}'],
                f'[intent] verbose: {label} printed as reported to the executable '
                f'({report[f"executable_address.{key}"]}): {found and found.group(0)!r}')
    for line in LOADING:
        t.check(line in text, f'verbose: {line!r} still printed')
    text, report = boot_messages(t, 'quiet: yes\n')
    t.check('limine' not in text.lower(), f'[intent] quiet: nothing printed to the serial line:\n{text}')
    t.expect_eq(report['cmdline'], 'messages', 'quiet: booted')


def uart_divisor(m, base=0x3f8):
    """The UART's divisor latch, read with the machine paused."""
    def inb(port):
        return int(m.hmp(f'i/b {port:#x}').rsplit('=', 1)[1], 16)

    m.qmp.cmd('stop')
    try:
        lcr = inb(base + 3)
        m.hmp(f'o/b {base + 3:#x} {lcr | 0x80:#x}')
        divisor = inb(base) | inb(base + 1) << 8
        m.hmp(f'o/b {base + 3:#x} {lcr:#x}')
    finally:
        m.qmp.cmd('cont')
    return divisor


@check(ports=['bios'], tags=['console'])
def console_serial_baudrate(t):
    """serial_baudrate sets the rate serial I/O runs at: 115200 by default
    and at most, 50 at least, and only rates dividing 115200; anything else
    is 115200. Without serial: yes it is not used."""
    m = boot_menu(t, tree_menu('timeout: no\n'))
    t.wait_text(m, 'ARROWS')
    firmware = uart_divisor(m)
    m.stop()
    for value, divisor in ((None, 1), ('9600', 12), ('50', 2304), ('300', 384), ('110', 1), ('56000', 1),
                           ('230400', 1), ('25', 1), ('fast', 1)):
        option = '' if value is None else f'serial_baudrate: {value}\n'
        m = boot_menu(t, tree_menu('timeout: no\nserial: yes\n' + option))
        t.wait_serial_text(m, 'ARROWS')
        t.expect_eq(uart_divisor(m), divisor, f'[spec] serial_baudrate: {value}: rate 115200/{divisor}')
        m.stop()
    m = boot_menu(t, tree_menu('timeout: no\nserial_baudrate: 9600\n'))
    t.wait_text(m, 'ARROWS')
    t.expect_eq(uart_divisor(m), firmware, '[spec] without serial: yes the UART is left as the firmware set it')


def esc(seq):
    return b'\x1b' + seq


# On UEFI the firmware's terminal decodes the serial line, passing on as
# characters what it does not know. The RISC-V and LoongArch edk2 firmware
# instead drops the digits of the numbered keys it does not know, leaving
# ESC [ ~ for VT220's 1, 4, 7 and 8 among them.
DIGIT_DROPPING_PORTS = ['uefi-riscv64', 'uefi-loongarch64']


@check(ports=[p for p in ALL_PORTS if p not in DIGIT_DROPPING_PORTS], tags=['console', 'serial'])
def console_serial_escape_sequences(t):
    """Keys typed on the serial line as the escape sequences terminals send
    (ECMA-48 5th edition control sequences, VT220's numbered ~ keys, xterm's
    SS3 and modifier parameters, rxvt's $ and ^ finals) work in the editor
    [intent]: Home and End by number (1, 4, 7, 8) and by SS3, Delete (3, and
    rxvt's shifted 3$), arrows with modifiers, PgUp and PgDn, F10 (21) to
    boot; a number past any key's is ignored."""
    m = boot_menu(t, 'timeout: no\nserial: yes\n/Only\n    protocol: limine\n    cmdline: abcdef\n'
                  f'    path: {KERNEL}\n')
    t.wait_serial_text(m, 'ARROWS')
    type_serial(m, b'e')
    t.wait_serial_text(m, 'cmdline: abcdef')
    type_serial(m, esc(b'[B'), esc(b'[1;5B'), esc(b'[1;2A'),  # down twice, once with Ctrl; Shift+Up
                esc(b'[4~'), b'X',                             # End: abcdefX
                esc(b'[1~'), *[esc(b'[C')] * 9,               # Home, then to the a
                esc(b'[3~'), esc(b'[3$'),                      # Delete twice: cdefX
                esc(b'[8~'), b'Y',                             # rxvt End: cdefXY
                esc(b'[7~'), esc(b'OF'), b'Z',                 # rxvt Home, SS3 End: cdefXYZ
                esc(b'[5~'), esc(b'[6~'),                      # PgUp, PgDn: nowhere to go
                esc(b'[99999999999~'),                         # no such key
                esc(b'[21~'))                                  # F10
    t.expect_eq(booted_cmdline(t, m), 'cdefXYZ', 'the edited entry booted')


@check(ports=[p for p in UEFI_PORTS if p not in DIGIT_DROPPING_PORTS], tags=['console', 'serial'])
def console_serial_split_sequences(t):
    """[intent] A key's escape sequence whose bytes come apart, as the
    firmware's console can pass them on over several of its polls of the
    serial line, is still the one key: Down, Ctrl+Down, Shift+Up, End, and
    F10, each sent a byte every 50ms."""
    m = boot_menu(t, 'timeout: no\nserial: yes\n/Only\n    protocol: limine\n    cmdline: abcdef\n'
                  f'    path: {KERNEL}\n')
    t.wait_serial_text(m, 'ARROWS')
    type_serial(m, b'e')
    t.wait_serial_text(m, 'cmdline: abcdef')
    for key in (esc(b'[B'), esc(b'[1;5B'), esc(b'[1;2A'), esc(b'[4~'), b'X', esc(b'[21~')):
        for byte in key:
            m.stream('serial').write(bytes([byte]))
            time.sleep(0.05)
        time.sleep(0.3)
    t.expect_eq(booted_cmdline(t, m), 'abcdefX', 'the edited entry booted')


@check(ports=DIGIT_DROPPING_PORTS, tags=['console', 'serial'])
def console_serial_decoded_keys(t):
    """Keys that firmware's terminal decodes work in the editor: Home and End
    as control sequence (ECMA-48 5th edition) and SS3 finals, Backspace sent
    as DEL, PgUp and PgDn, and F10 to boot."""
    m = boot_menu(t, 'timeout: no\nserial: yes\n/Only\n    protocol: limine\n    cmdline: abcdef\n'
                  f'    path: {KERNEL}\n')
    t.wait_serial_text(m, 'ARROWS')
    type_serial(m, b'e')
    t.wait_serial_text(m, 'cmdline: abcdef')
    type_serial(m, esc(b'[B'),                                 # onto the cmdline line
                esc(b'[F'), b'X',                              # End: abcdefX
                esc(b'[H'), *[esc(b'[C')] * 11,               # Home, then past the b
                b'\x7f', b'\x7f',                              # Backspace twice: cdefX
                esc(b'OF'), b'Y',                              # SS3 End: cdefXY
                esc(b'OH'), esc(b'[F'), b'Z',                  # SS3 Home, End: cdefXYZ
                esc(b'[5~'), esc(b'[6~'),                      # PgUp, PgDn: nowhere to go
                esc(b'[21~'))                                  # F10
    t.expect_eq(booted_cmdline(t, m), 'cdefXYZ', 'the edited entry booted')
