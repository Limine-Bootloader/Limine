import time

from harness.api import check
from harness.media import HostFile
from harness.ports import ALL_PORTS, UEFI_PORTS
from harness.screen import Font
from cases._common import screen_text, wait_panic
from cases.menu import (KERNEL, boot_menu, booted_cmdline, entry_cells, menu_entry, nothing_booted, tree_menu,
                        wait_menu)

# The menu and the editor at their edges: what does not fit the screen, keys
# beyond the arrows, entries that cannot boot. What the code alone decides is
# [intent]; Limine's terminal draws the box and arrow characters from CP437.

# Limine's terminal palette for the editor's ANSI colours.
CYAN, GREEN, RED, BROWN, GREY = 0x00aaaa, 0x00aa00, 0xaa0000, 0xaa5500, 0xaaaaaa


def selected(scr, name):
    cells = entry_cells(scr, name)
    return cells is not None and cells[1].bg != scr.cell(0, scr.find(f' {name} ')[0][1]).bg


@check(ports=ALL_PORTS, tags=['menu'])
def menu_number_keys(t):
    """A digit boots the entry of that number, counting what is shown, and
    one past the last does nothing [intent]."""
    m = boot_menu(t, tree_menu())
    wait_menu(t, m)
    m.press('9')
    t.check(nothing_booted(t, m, 2), 'no ninth entry: nothing booted')
    m.press('6')
    t.expect_eq(booted_cmdline(t, m), 'last', 'the sixth entry shown (Last) booted')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_home_end_wrap(t):
    """End selects the last entry, Home the first, Up from the first wraps to
    the last and Down from the last back to the first [intent]."""
    m = boot_menu(t, tree_menu())
    wait_menu(t, m)
    m.press('end')
    t.wait_screen(m, lambda s: selected(s, 'Last'), desc='End: Last selected')
    m.press('home')
    t.wait_screen(m, lambda s: selected(s, 'First'), desc='Home: First selected')
    m.press('up')
    t.wait_screen(m, lambda s: selected(s, 'Last'), desc='Up from First: Last selected')
    m.press('down')
    t.wait_screen(m, lambda s: selected(s, 'First'), desc='Down from Last: First selected')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_overflow(t):
    """More entries than rows: arrows above and below the list show there is
    more [intent]; a name, a comment, or a branding too long for the screen
    is cut short with an ellipsis [intent]."""
    long_name = 'Long' + 'n' * 200
    config = ('timeout: no\ninterface_branding: ' + 'B' * 300 + '\n'
              + menu_entry(1, long_name, 'long', 'C' * 300)
              + ''.join(menu_entry(1, f'Entry {i:02}', f'entry {i}') for i in range(60)))
    m = boot_menu(t, config)
    scr = wait_menu(t, m)
    text = screen_text(scr)
    t.check('↓↓↓' in text and '↑↑↑' not in text, 'more below, nothing above')
    t.check(any(row.rstrip().endswith('...') and 'Longnnn' in row for row in scr.lines()), 'long name cut short')
    t.check(any(row.rstrip().endswith('...') and 'CCC' in row for row in scr.lines()), 'long comment cut short')
    t.check(any(row.rstrip().endswith('...') and 'BBB' in row for row in scr.lines()), 'long branding cut short')
    m.press('end')
    scr = t.wait_text(m, 'Entry 59')
    t.check('↑↑↑' in screen_text(scr), 'at the end: more above')
    m.press('ret')
    t.expect_eq(booted_cmdline(t, m), 'entry 59', 'the last entry booted')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_blank_entry(t):
    """B opens the editor on a blank entry, which F10 boots [intent]; with no
    entries configured the menu still offers it."""
    for config in (tree_menu(), 'timeout: no\n'):
        m = boot_menu(t, config)
        t.wait_text(m, 'B Blank Entry' if 'First' in config else 'Blank')
        m.press('b')
        t.wait_text(m, 'Blank Entry')
        m.type_text(f'protocol: limine\npath: {KERNEL}\ncmdline: typed in')
        t.wait_text(m, 'cmdline: typed in')
        m.press('f10')
        t.expect_eq(booted_cmdline(t, m), 'typed in', 'the typed entry booted')
        m.stop()


@check(ports=ALL_PORTS, tags=['menu', 'hostile'])
def menu_protocol_errors(t):
    """An entry without a protocol, or with one Limine does not know or this
    port does not boot, is refused, and the menu comes back; one for the
    other firmware's chainloading is not listed at all [intent]."""
    foreign = 'efi' if t.port.name == 'bios' else 'bios'
    cases = [('/No protocol\n    path: boot():/boot/kernel.elf\n', 'Boot protocol not specified for this entry'),
             ('/Unknown\n    protocol: nonsense\n', 'Unsupported protocol specified.')]
    if not t.port.is_x86:
        cases += [('/MB1\n    protocol: multiboot1\n', 'Unsupported protocol specified.'),
                  ('/MB2\n    protocol: multiboot2\n', 'Unsupported protocol specified.')]
    m = boot_menu(t, 'timeout: no\n' + ''.join(text for text, _ in cases)
                  + f'/Foreign\n    protocol: {foreign}\n')
    t.check('Foreign' not in screen_text(wait_menu(t, m)), f'the {foreign} entry not listed')
    for i, (text, want) in enumerate(cases):
        wait_menu(t, m)
        m.press('home', *['down'] * i, delay=0.05)
        m.press('ret')
        msg, scr = wait_panic(t, m)
        t.expect_eq(msg, want, text.split('\n')[0])
        if 'multiboot' in text:
            t.check('is not available on non-x86 architectures' in screen_text(scr), 'says why')
        m.press('ret')


def editor_entry(cmdline, extra=''):
    return f'/Only\n    protocol: limine\n    path: {KERNEL}\n    cmdline: {cmdline}\n{extra}'


@check(ports=ALL_PORTS, tags=['menu'])
def menu_editor_long_lines(t):
    """A line wider than the editor wraps, an arrow at the end of each row
    and at the start of the next [intent]; it edits as one line."""
    cmdline = ''.join(f'{i:03}-' for i in range(60))
    m = boot_menu(t, 'timeout: no\n' + editor_entry(cmdline))
    wait_menu(t, m)
    m.press('e')
    scr = t.wait_text(m, 'cmdline: 000-')
    rows = scr.lines()
    t.check(any(row.rstrip().endswith('→') for row in rows), 'a row ends with →')
    t.check(any(row.lstrip().startswith('←') or row.startswith('←') for row in rows), 'the next starts with ←')
    m.press('down', 'down', 'end')
    m.type_text('X')
    m.press('f10')
    t.expect_eq(booted_cmdline(t, m), cmdline + 'X', 'the whole line booted, edited at its end')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_editor_tabs(t):
    """A tab shows as spaces to the next multiple of 8 columns [intent]."""
    m = boot_menu(t, 'timeout: no\n/Only\n\tprotocol: limine\n\tpath: ' + KERNEL + '\n\tcmdline: x\ty\n')
    wait_menu(t, m)
    m.press('e')
    scr = t.wait_text(m, 'cmdline: x')
    x, y = scr.find_one('cmdline: x')
    t.expect_eq(scr.row_text(y)[x:x + 17], 'cmdline: x      y', 'x then spaces to column 16, then y')
    m.press('f10')
    t.expect_eq(booted_cmdline(t, m), 'x\ty', 'the tab kept')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_editor_scrolling(t):
    """An entry longer than the editor window scrolls with the cursor, arrows
    in the frame showing what is out of view [intent]."""
    comments = ''.join(f'    # comment line {i:02}\n' for i in range(60))
    m = boot_menu(t, 'timeout: no\n' + editor_entry('scrolled', comments))
    wait_menu(t, m)
    m.press('e')
    scr = t.wait_text(m, 'cmdline: scrolled')
    t.check('comment line 59' not in screen_text(scr), 'the last line out of view')
    bottom = [row for row in scr.lines() if '┘' in row]
    t.check(bottom and '↓' in bottom[0], 'more below: ↓ in the bottom of the frame')
    m.press(*['down'] * 70, delay=0.06)
    scr = t.wait_text(m, 'comment line 59')
    top = [row for row in scr.lines() if '┐' in row]
    t.check(top and '↑' in top[0], 'more above: ↑ in the top of the frame')
    m.press(*['up'] * 70, delay=0.06)
    scr = t.wait_text(m, 'cmdline: scrolled')
    m.press('f10')
    t.expect_eq(booted_cmdline(t, m), 'scrolled', 'booted unchanged')


@check(ports=ALL_PORTS, tags=['menu', 'hostile'])
def menu_editor_limits(t):
    """The editor holds 4095 characters: a larger entry is refused [intent],
    and typing into a full buffer says so instead of growing it."""
    big = editor_entry('big', '    # ' + 'x' * 4100 + '\n')
    fill = 4095 - len(editor_entry('full').lstrip('/Only\n').rstrip('\n')) - 4
    full = editor_entry('full', '    #' + 'y' * fill + '\n')
    m = boot_menu(t, 'timeout: no\n' + big + full.replace('/Only', '/Full'))
    wait_menu(t, m)
    m.press('e')
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'Entry is too big to be edited.', 'an entry past the editor buffer')
    m.press('ret')
    wait_menu(t, m)
    m.press('down', 'e')
    t.wait_text(m, 'cmdline: full')
    m.type_text('zzzzzzzzzzzzzzzzzzzz')
    scr = t.wait_text(m, 'Buffer full')
    t.check('Buffer full' in screen_text(scr), 'the frame says the buffer is full')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_editor_highlighting(t):
    """The editor colours a known key cyan, its colon green, a value in the
    default colour, a key Limine does not know red, and a comment brown
    [intent]; editor_validation: no colours every key as known, and
    editor_highlighting: no colours nothing."""
    entry = editor_entry('v', '    bogus_key: 1\n    # a comment\n')

    def colours(scr):
        x, y = scr.find_one('cmdline: v')
        bx, by = scr.find_one('bogus_key')
        cx, cy = scr.find_one('# a comment')
        return (scr.cell(x, y).fg, scr.cell(x + 7, y).fg, scr.cell(x + 9, y).fg, scr.cell(bx, by).fg,
                scr.cell(cx, cy).fg)

    for globals_, want in (('', (CYAN, GREEN, GREY, RED, BROWN)),
                           ('editor_validation: no\n', (CYAN, GREEN, GREY, CYAN, CYAN)),
                           ('editor_highlighting: no\n', (GREY,) * 5)):
        m = boot_menu(t, 'timeout: no\n' + globals_ + entry)
        wait_menu(t, m)
        m.press('e')
        scr = t.wait_text(m, '# a comment')
        time.sleep(0.5)
        t.expect_eq(colours(t.screen(m)), want, globals_.strip() or 'default highlighting')
        m.stop()


@check(ports=ALL_PORTS, tags=['menu'])
def menu_editor_tall_line(t):
    """A line taller than the editor window: with the cursor at its end the
    window begins partway through it, its first row marked as continuing
    [intent]; without highlighting the rows wrap the same."""
    cmdline = ''.join(f'{i:04}-' for i in range(600))
    m = boot_menu(t, 'timeout: no\ninterface_resolution: 640x480\neditor_highlighting: no\n' + editor_entry(cmdline))
    wait_menu(t, m)
    m.press('e')
    t.wait_text(m, 'cmdline: 0000-')
    m.press('down', 'down', 'end')
    rows = t.wait_text(m, '0599-').lines()
    top = next((i for i, row in enumerate(rows) if '┐' in row), None)
    first = rows[top + 1] if top is not None else ''
    t.check(first[:1] == '←', f'the window begins inside the line: {first!r}')
    m.type_text('X')
    m.press('f10')
    t.expect_eq(booted_cmdline(t, m), cmdline + 'X', 'the whole line booted, edited at its end')


@check(ports=ALL_PORTS, tags=['menu', 'hostile'])
def menu_branding_measure(t):
    """[user] The branding is measured by what it shows, in the menu as in
    the editor: an escape sequence takes no room, and a character one cell
    however many bytes UTF-8 gives it. A branding that fits is centred
    whole; one that does not is cut to the screen's width less two, "..."
    included, between characters."""
    only = menu_entry(1, 'Only', 'only')
    m = boot_menu(t, 'timeout: no\n' + only)
    cols = wait_menu(t, m).cols
    m.stop()

    # Black, 30, would hide its character on the screen's black.
    def coloured(text):
        return ''.join(f'\x1b[3{1 + i % 7}m{ch}' for i, ch in enumerate(text)) + '\x1b[0m'

    cases = (('short', coloured('Café Brand'), 'Café Brand', (cols - 10) // 2),
             ('exact', coloured('é' * (cols - 2)), 'é' * (cols - 2), 1),
             ('long', coloured('é' * (cols + 3)), 'é' * (cols - 5) + '...', 1))
    for what, branding, shown, x in cases:
        m = boot_menu(t, f'timeout: no\ninterface_branding: {branding}\n' + only)
        for where, scr in (('menu', wait_menu(t, m)), ('editor', None)):
            if scr is None:
                m.press('e')
                scr = t.wait_text(m, 'cmdline: only')
            hits = [(hx, hy) for hx, hy in scr.find(shown) if scr.row_text(hy).strip() == shown]
            t.check(len(hits) == 1 and hits[0][0] == x,
                    f'{what}, {where}: {shown!r} alone on its row from column {x}: {hits}\n{scr.text()}')
        m.stop()


@check(ports=ALL_PORTS, tags=['menu', 'hostile'])
def menu_comment_measure(t):
    """[user] An entry's comment is measured by what it shows, as the
    branding is: an escape sequence takes no room, and a character one cell
    however many bytes UTF-8 gives it. A comment that fits is centred whole;
    one that does not is cut to the screen's width less two, "..." included,
    between characters."""
    m = boot_menu(t, 'timeout: no\n' + menu_entry(1, 'Only', 'only'))
    cols = wait_menu(t, m).cols
    m.stop()

    # Black, 30, would hide its character on the screen's black.
    def coloured(text):
        return ''.join(f'\x1b[3{1 + i % 7}m{ch}' for i, ch in enumerate(text)) + '\x1b[0m'

    cases = (('short', coloured('Café Comment'), 'Café Comment', (cols - 12) // 2),
             ('exact', coloured('é' * (cols - 2)), 'é' * (cols - 2), 1),
             ('long', coloured('é' * (cols + 3)), 'é' * (cols - 5) + '...', 1))
    for what, comment, shown, x in cases:
        m = boot_menu(t, 'timeout: no\n' + menu_entry(1, 'Only', 'only', comment))
        scr = t.wait_text(m, shown)
        hits = [(hx, hy) for hx, hy in scr.find(shown) if scr.row_text(hy).strip() == shown]
        t.check(len(hits) == 1 and hits[0][0] == x,
                f'{what}: {shown!r} alone on its row from column {x}: {hits}\n{scr.text()}')
        m.stop()


@check(ports=ALL_PORTS, tags=['menu', 'hostile'])
def menu_long_names(t):
    """Names wider than the screen are cut short with "...": the branding, an
    entry's title in the editor's frame, and the title the editor keeps for
    an entry it booted, which a failed boot brings back; the blank entry's
    editor comes back the same way, and Esc leaves either [intent]."""
    m = boot_menu(t, 'timeout: no\ninterface_branding: ' + 'B' * 300 + '\n'
                  + f'/{"N" * 300}\n    path: {KERNEL}\n    cmdline: named\n')
    text = screen_text(wait_menu(t, m))
    t.check('BBB...' in text and 'B' * 300 not in text, 'the branding cut short')
    m.press('e')
    t.check('NNN...' in screen_text(t.wait_text(m, 'cmdline: named')), 'the title cut short in the frame')
    for typed, back in (('', 'cmdline: named'), ('cmdline: blank', 'cmdline: blank')):
        if typed:
            m.press('b')
            t.wait_text(m, 'Blank Entry')
            m.type_text(typed)
        m.press('f10')
        msg, _ = wait_panic(t, m)
        t.expect_eq(msg, 'Boot protocol not specified for this entry', f'{back}: refused')
        m.press('ret')
        scr = t.wait_text(m, back)
        if not typed:
            t.check('NNN...' in screen_text(scr), 'back in the editor, the kept title cut short too')
        m.press('esc')
        wait_menu(t, m)


@check(ports=ALL_PORTS, tags=['menu'])
def menu_deep_tree(t):
    """Directories three deep draw their branches: a line down past an
    ancestor with entries after it, a gap past one without [intent]."""
    config = ('timeout: no\n' + menu_entry(1, '+Outer') + menu_entry(2, '+Middle') + menu_entry(3, 'Inner', 'inner')
              + menu_entry(2, 'Other', 'other') + menu_entry(2, '+Last') + menu_entry(3, 'Deep', 'deep'))
    m = boot_menu(t, config)
    rows = wait_menu(t, m).lines()
    inner = next((r[:r.index('Inner')] for r in rows if 'Inner' in r), '')
    deep = next((r[:r.index('Deep')] for r in rows if 'Deep' in r), '')
    t.check(inner.count('│') == deep.count('│') + 1, f'a line past Middle, a gap past Last: {inner!r} {deep!r}')
    m.press('end', 'ret')
    t.expect_eq(booted_cmdline(t, m), 'deep', 'the deepest entry boots')


@check(ports=UEFI_PORTS, tags=['menu'])
def menu_narrow_help(t):
    """[intent] Where the actions after the entries do not fit the width,
    their labels shorten: Setup, Shell, Blank. In full, with a shell to
    offer, they would take 49 columns of the 44 here."""
    from cases.uefi import probe_file
    config = 'timeout: no\ninterface_resolution: 800x600\nterm_margin: 0\nterm_font_scale: 2x1\n'
    m = boot_menu(t, config + menu_entry(1, 'Only', 'only'),
                  files={'/' + t.port.shell_name.upper(): HostFile(probe_file(t))})
    text = screen_text(t.wait_text(m, 'Blank', font=Font(scale_x=2)))
    t.check(all(label in text for label in ('S Setup', 'U Shell', 'B Blank')) and 'Blank Entry' not in text,
            f'short labels: {text!r}')
