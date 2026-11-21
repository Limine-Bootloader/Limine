from harness.api import check
from harness.ports import ALL_PORTS, UEFI_PORTS
from cases.console import serial_colour
from cases.menu import boot_menu, booted_cmdline, menu_entry, wait_menu

# Limine's terminal interprets control sequences (ECMA-48 5th edition) in what
# it prints, config strings included: here, in the branding. In VGA text mode
# colours are attribute nibbles, bright meaning the intensity bit [spec: the
# VGA registers volume of Intel's 2011 graphics PRM, revision 1.0, SR03 and
# AR10]; the graphical terminal's are its palette's.

E = '\x1b'
# Two brandings, each short enough not to be cut short: the cut counts the
# bytes of the sequences as well.
COLOURS = (f'{E}[0mA{E}[1mB{E}[0m{E}[5mC{E}[0m{E}[93mD{E}[101mE{E}[0m{E}[34;1mF{E}[0m{E}[48;2;1;2;3mG'
           f'{E}[0m')
# A 24-bit green nearest the palette's green, and a pink nearest its light red.
RGB = f'{E}[38;2;0;200;0mH{E}[48;2;250;80;80mI{E}[0m'
EDITS = f'{E}7ABC{E}8D{E}[2CPQ{E}[2D{E}[2@R{E}[3CSTU{E}[3D{E}[P{E}[0m'
# What the edits leave: D written over A where the cursor was saved, R and a
# blank, of the two inserted, before PQ, and S deleted from STU.
EDITED = 'DBCR PQTU'
VGA_GREY, VGA_WHITE, VGA_DARK_GREY, VGA_GREEN = 0xaaaaaa, 0xffffff, 0x555555, 0x00aa00
VGA_YELLOW, VGA_LIGHT_RED, VGA_LIGHT_BLUE, VGA_BLACK = 0xffff55, 0xff5555, 0x5555ff, 0x000000


def branding_at(t, graphics, branding, shown):
    """The cells of what a branding shows, by character, and its row."""
    m = boot_menu(t, f'timeout: no\ngraphics: {graphics}\ninterface_branding: {branding}\n'
                  + menu_entry(1, 'Only', 'x'))
    scr = wait_menu(t, m)
    x, y = scr.find_one(shown)
    m.stop()
    return {ch: scr.cell(x + i, y) for i, ch in enumerate(shown)}, y


def branding_cells(t, graphics, branding, shown):
    return branding_at(t, graphics, branding, shown)[0]


@check(ports=['bios'], tags=['console', 'term'])
def term_escapes_textmode(t):
    """In VGA text mode: SGR 1 brightens the default foreground, SGR 5 the
    default background, 90-97 and 100-107 are the bright colours, bold
    brightens a colour set before it, a 24-bit colour shows as the nearest
    of the 16 [intent]; DECSC and DECRC save and restore the cursor, CUF and
    CUB move it, ICH inserts blanks, DCH deletes."""
    branding_cells(t, 'no', EDITS, EDITED)
    c = branding_cells(t, 'no', COLOURS, 'ABCDEFG')
    c.update(branding_cells(t, 'no', RGB, 'HI'))
    for ch, attr, want in (('A', 'fg', VGA_GREY), ('B', 'fg', VGA_WHITE), ('C', 'bg', VGA_DARK_GREY),
                           ('D', 'fg', VGA_YELLOW), ('E', 'fg', VGA_YELLOW), ('E', 'bg', VGA_LIGHT_RED),
                           ('F', 'fg', VGA_LIGHT_BLUE), ('G', 'bg', VGA_BLACK), ('H', 'fg', VGA_GREEN),
                           ('I', 'fg', VGA_GREEN), ('I', 'bg', VGA_LIGHT_RED)):
        t.expect_eq(getattr(c[ch], attr), want, f'{ch} {attr}')
    # RI with the cursor homed to the top row scrolls the screen down a row.
    _, row = branding_at(t, 'no', 'REV', 'REV')
    _, moved = branding_at(t, 'no', f'REV{E}[H{E}M', 'REV')
    t.expect_eq(moved, row + 1, 'reverse index at the top scrolled the branding down a row')


@check(ports=['bios'], tags=['console', 'term'])
def term_textmode_verbose_boot(t):
    """verbose: yes in text mode: more lines than the screen has scroll it,
    and the entry boots."""
    m = boot_menu(t, 'timeout: 0\ngraphics: no\nverbose: yes\n' + menu_entry(1, 'Only', 'verbose'))
    t.expect_eq(booted_cmdline(t, m), 'verbose', 'booted')


@check(ports=ALL_PORTS, tags=['console', 'term'])
def term_escapes_graphical(t):
    """The same in the graphical terminal: the cursor and editing sequences
    the same, bright colours brighter than the normal ones, and a 24-bit
    background as given [intent]."""
    branding_cells(t, 'yes', EDITS, EDITED)
    c = branding_cells(t, 'yes', COLOURS, 'ABCDEFG')
    t.check(c['B'].fg != c['A'].fg and c['C'].bg != c['A'].bg, 'bold and blink brighten the defaults')
    t.check(c['E'].bg != c['A'].bg and c['D'].fg == c['E'].fg, 'bright red background behind E')
    t.expect_eq(c['G'].bg, 0x010203, '24-bit background')


@check(ports=UEFI_PORTS, tags=['console', 'term'])
def term_escapes_firmware_console(t):
    """On the firmware's console, as the serial line gives it, SGR 30-37 and
    90-97 are each of the 16 colours, 33 brown and 93 yellow as in VGA text
    mode [intent]."""
    for base, shown in ((0, '01234567'), (8, '89ABCDEF')):
        first = 30 if base == 0 else 90
        branding = ''.join(f'{E}[{first + i}m{ch}' for i, ch in enumerate(shown)) + f'{E}[0m'
        m = boot_menu(t, f'timeout: no\nserial: yes\ninterface_branding: {branding}\n' + menu_entry(1, 'Only', 'x'))
        t.wait_serial_text(m, shown)
        t.expect_eq([serial_colour(t, m, shown, i) for i in range(8)], [f'c{base + i}' for i in range(8)],
                    f'SGR {first}-{first + 7}')
        m.stop()


@check(ports=ALL_PORTS, tags=['console', 'term'])
def term_too_small_falls_back(t):
    """A terminal too small for the menu (here, a font scaled up fourfold)
    gives way to the text console: VGA text mode on BIOS, the firmware's
    console on UEFI, drawn in ASCII [intent]; colours and the rest still
    work there, and so does booting, verbose output scrolling it."""
    config = (f'timeout: no\nverbose: yes\nterm_font_scale: 4x4\ninterface_branding: {COLOURS}\n'
              + menu_entry(1, 'Only', 'fallback'))
    m = boot_menu(t, config)
    if t.port.is_uefi:
        scr = t.wait_screen(m, lambda s: 'ABCDEFG' in s.text() and 'Only' in s.text(), firmware=True,
                            desc='the menu on the firmware console')
    else:
        scr = wait_menu(t, m)
    t.check('ABCDEFG' in scr.text() and 'Only' in scr.text(), 'the menu shown')
    m.press('ret')
    t.expect_eq(booted_cmdline(t, m), 'fallback', 'booted from it')


@check(ports=UEFI_PORTS, tags=['console', 'term'])
def term_firmware_console_cursor(t):
    """On the firmware's console, here for serial: yes, the firmware's cursor
    shows where Limine's terminal has one: in the editor, not in the menu
    [intent]."""
    m = boot_menu(t, 'timeout: no\nserial: yes\n' + menu_entry(1, 'Only', 'x'))
    scr = t.wait_screen(m, lambda s: 'ARROWS' in s.text(), firmware=True, desc='the menu')
    t.expect_eq(scr.cursors(), [], 'no cursor in the menu')
    m.press('e')
    scr = t.wait_screen(m, lambda s: 'cmdline: x' in s.text(), firmware=True, desc='the editor')
    t.expect_eq(len(scr.cursors()), 1, 'a cursor in the editor')
    m.press('esc')
    scr = t.wait_screen(m, lambda s: 'ARROWS' in s.text(), firmware=True, desc='the menu again')
    t.expect_eq(scr.cursors(), [], 'none once back in the menu')
