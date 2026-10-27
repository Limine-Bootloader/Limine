from harness.api import check
from harness.ports import ALL_PORTS
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
EDITS = f'{E}7ABC{E}8D{E}[2CPQ{E}[2D{E}[2@R{E}[3CSTU{E}[3D{E}[P{E}[0m'
# What the edits leave: D written over A where the cursor was saved, R and a
# blank, of the two inserted, before PQ, and S deleted from STU.
EDITED = 'DBCR PQTU'
VGA_GREY, VGA_WHITE, VGA_DARK_GREY = 0xaaaaaa, 0xffffff, 0x555555
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
    brightens a colour set before it, a 24-bit colour is ignored; DECSC and
    DECRC save and restore the cursor, CUF and CUB move it, ICH inserts
    blanks, DCH deletes."""
    branding_cells(t, 'no', EDITS, EDITED)
    c = branding_cells(t, 'no', COLOURS, 'ABCDEFG')
    for ch, attr, want in (('A', 'fg', VGA_GREY), ('B', 'fg', VGA_WHITE), ('C', 'bg', VGA_DARK_GREY),
                           ('D', 'fg', VGA_YELLOW), ('E', 'fg', VGA_YELLOW), ('E', 'bg', VGA_LIGHT_RED),
                           ('F', 'fg', VGA_LIGHT_BLUE), ('G', 'bg', VGA_BLACK)):
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
