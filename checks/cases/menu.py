import time

from harness.api import check
from harness import efivars
from harness.media import HostFile, Tree
from harness.ports import ALL_PORTS, UEFI_PORTS
from harness.report import unescape
from cases._common import kernel_file, limine_version, screen_text

# The boot menu, seen on screen and driven from the keyboard. Expectations
# come from CONFIG.md; [intent] marks what only Limine's code says, such as
# its default branding and help text.

KERNEL = 'boot():/boot/kernel.elf'
# The vendor of the variables Limine keeps across boots.
LIMINE_VENDOR = '03884334-1756-4978-9c8c-86fb8998b4ea'
BRANDING_ARCH = {'bios': 'x86-64, BIOS', 'uefi-ia32': 'x86-64, UEFI32', 'uefi-x86-64': 'x86-64, UEFI',
                 'uefi-aarch64': 'aarch64, UEFI', 'uefi-riscv64': 'riscv64, UEFI',
                 'uefi-loongarch64': 'loongarch64, UEFI'}


def menu_entry(level, title, cmdline=None, comment=None):
    lines = ['/' * level + title]
    if cmdline is not None:
        lines += ['    protocol: limine', f'    path: {KERNEL}', f'    cmdline: {cmdline}']
    if comment is not None:
        lines.append(f'    comment: {comment}')
    return '\n'.join(lines) + '\n'


def tree_menu(globals_='timeout: no\n'):
    return (globals_
            + menu_entry(1, 'First', 'first', 'the first comment')
            + menu_entry(1, '+Dir')
            + menu_entry(2, 'Inner One', 'inner one', 'an inner comment')
            + menu_entry(2, 'Inner Two', 'inner two')
            + menu_entry(1, 'Collapsed')
            + menu_entry(2, 'Hidden Child', 'hidden child')
            + menu_entry(1, 'Last', 'last'))


def boot_menu(t, config, files=None, **machine):
    tree = Tree()
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
    for path, data in (files or {}).items():
        tree.add(path, data)
    t.limine_files(config, tree)
    return t.boot(t.boot_disk(tree), **machine)


def wait_menu(t, m, needle='ARROWS', timeout=30):
    return t.wait_text(m, needle, timeout)


def booted_cmdline(t, m, timeout=60):
    return unescape(t.kernel_report(m, timeout=timeout)['cmdline'])


def entry_cells(scr, name):
    """The cells of an entry's name as the menu prints it: ' name '."""
    hits = scr.find(f' {name} ')
    if not hits:
        return None
    x, y = hits[0]
    return [scr.cell(x + i, y) for i in range(len(name) + 2)]


def nothing_booted(t, m, seconds):
    time.sleep(seconds * t.port.timeout_scale)
    return 'B LIMINE-TEST' not in m.stream(t.kernel_channel).text()


@check(ports=ALL_PORTS, tags=['menu'])
def menu_layout(t):
    """The menu lists entries, '+' directories expanded, others collapsed,
    the default entry highlighted with its comment shown."""
    m = boot_menu(t, tree_menu())
    scr = wait_menu(t, m)
    text = screen_text(scr)
    branding = f'Limine {limine_version(t.env)} ({BRANDING_ARCH[t.port.name]})'
    _, brand_row = scr.find_one(branding)
    _, help_row = scr.find_one('ARROWS Select    ENTER Boot    E Edit')
    t.check(brand_row < help_row, '[intent] default branding at the top, above the key help')
    for name in ('First', '[-] Dir', 'Inner One', 'Inner Two', '[+] Collapsed', 'Last'):
        t.check(name in text, f'entry {name!r} listed')
    t.check('Hidden Child' not in text, 'entries in a collapsed directory hidden')
    t.check('the first comment' in text, 'the selected entry\'s comment shown')
    t.check('Booting automatically' not in text, 'no countdown with timeout: no')
    selected, other = entry_cells(scr, 'First'), entry_cells(scr, 'Last')
    t.require(selected and other, 'entries found on screen')
    t.expect_eq((selected[1].fg, selected[1].bg), (other[1].bg, other[1].fg),
                '[intent] the default entry is shown in inverse video')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_keyboard(t):
    """Arrows move the selection, its comment follows, and Enter boots it."""
    m = boot_menu(t, tree_menu())
    wait_menu(t, m)
    m.press('down', 'down')
    scr = t.wait_text(m, 'an inner comment')
    t.check(entry_cells(scr, 'Inner One')[1].bg != entry_cells(scr, 'First')[1].bg, 'selection moved to Inner One')
    m.press('down', 'down', 'down')
    t.wait_text(m, 'Last')
    m.press('ret')
    t.expect_eq(booted_cmdline(t, m), 'last', 'Enter booted the selected entry')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_directories(t):
    """Enter expands a collapsed directory and collapses an expanded one."""
    m = boot_menu(t, tree_menu())
    wait_menu(t, m)
    m.press('down', 'down', 'down', 'down')
    scr = t.wait_text(m, 'ENTER Expand')
    t.check(scr.find('[+] Collapsed'), 'on the collapsed directory')
    m.press('ret')
    scr = t.wait_text(m, 'Hidden Child')
    t.check(scr.find('[-] Collapsed') and scr.find('ENTER Collapse'), '[intent] expanded, help offers collapsing')
    m.press('up', 'up', 'up')
    t.wait_text(m, 'ENTER Collapse')
    m.press('ret')
    scr = t.wait_screen(m, lambda s: 'Inner One' not in screen_text(s), desc='Dir collapsed')
    t.check(scr.find('[+] Dir'), 'Dir collapsed')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_countdown(t):
    """With a timeout the default entry boots after the countdown; a key
    pressed during it stops it."""
    config = tree_menu('timeout: 3\n')
    m = boot_menu(t, config)
    scr = t.wait_text(m, 'Booting automatically in')
    t.check('Booting automatically in 3...' in screen_text(scr) or 'Booting automatically in 2...' in screen_text(scr),
            '[intent] countdown shown in seconds')
    t.expect_eq(booted_cmdline(t, m, timeout=20), 'first', 'the default entry booted when the countdown ran out')
    m.stop()
    m = boot_menu(t, tree_menu('timeout: 4\n'))
    t.wait_text(m, 'Booting automatically in')
    m.press('down')
    scr = t.wait_screen(m, lambda s: 'Booting automatically' not in screen_text(s), desc='countdown gone')
    t.check(nothing_booted(t, m, 6), 'a key press stopped the countdown')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_timeout_values(t):
    """timeout reads leading digits and an optional fraction, ignores what
    follows, caps at 9999, and treats a value without digits as 0."""
    for value, shown in (('10s', '10'), ('1e3', '1'), ('2,5', '2'), ('2.95', '2.95'), ('99999', '9999')):
        m = boot_menu(t, tree_menu(f'timeout: {value}\n'))
        scr = t.wait_text(m, 'Booting automatically in')
        t.check(f'Booting automatically in {shown}...' in screen_text(scr), f'timeout: {value} counts from {shown}')
        m.stop()
    m = boot_menu(t, tree_menu('timeout: No\n'))
    t.expect_eq(booted_cmdline(t, m), 'first', 'timeout: No is 0: booted at once')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_interface_options(t):
    """interface_branding and its colour, the help colour, and hiding help."""
    config = ('timeout: no\ninterface_branding: A Custom Brand\ninterface_branding_colour: ff0000\n'
              'interface_help_color: 0000ff\n' + menu_entry(1, 'Only', 'only'))
    m = boot_menu(t, config)
    scr = wait_menu(t, m)
    x, y = scr.find_one('A Custom Brand')
    t.expect_eq(scr.row_text(y).strip(), 'A Custom Brand', 'custom branding in place of the default')
    t.expect_eq(scr.cell(x, y).fg, 0xff0000, 'branding colour')
    x, y = scr.find_one('ARROWS')
    t.expect_eq(scr.cell(x, y).fg, 0x0000ff, 'help colour, through the American alias')
    m.stop()
    m = boot_menu(t, 'timeout: no\n' + menu_entry(1, 'Only', 'only'))
    scr = wait_menu(t, m)
    x, y = scr.find_one('ARROWS')
    t.expect_eq(scr.cell(x, y).fg, 0x00aa00, 'default help colour 00aa00')
    x, y = scr.find_one('Limine ')
    t.expect_eq(scr.cell(x, y).fg, 0x00aaaa, 'default branding colour 00aaaa')
    m.stop()
    m = boot_menu(t, 'timeout: no\ninterface_help_hidden: yes\n' + menu_entry(1, 'Only', 'only'))
    scr = t.wait_text(m, 'Only')
    time.sleep(1)
    t.check('ARROWS' not in screen_text(t.screen(m)), 'help hidden')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_editor(t):
    """E opens the selected entry in the editor; F10 boots the edited entry."""
    m = boot_menu(t, 'timeout: no\n' + menu_entry(1, 'Only', 'original'))
    wait_menu(t, m)
    m.press('e')
    scr = t.wait_text(m, 'cmdline: original')
    t.check('F10/CTRL+X Boot' in screen_text(scr), '[intent] editor key help')
    # The cursor starts on the first line; down to cmdline, then to its end.
    m.press('down', 'down', 'end')
    m.type_text(' edited')
    t.wait_text(m, 'cmdline: original edited')
    m.press('f10')
    t.expect_eq(booted_cmdline(t, m), 'original edited', 'the edited entry booted')


# The cmdline line comes before the longer path line, so a cursor move gone
# wrong edits the path, and nothing boots.
EDITOR_ENTRY = '/Only\n    protocol: limine\n    cmdline: abcdef\n    path: boot():/boot/kernel.elf\n'


@check(ports=ALL_PORTS, tags=['menu'])
def menu_editor_keys(t):
    """The editor's keys [intent]: Up goes to the line above, as far along
    as it allows, Home and End to a line's ends, Left and Right a character
    over; Delete and Backspace take out the character after and before the
    cursor; CTRL+X boots, as the help says."""
    m = boot_menu(t, 'timeout: no\n' + EDITOR_ENTRY)
    wait_menu(t, m)
    m.press('e')
    t.wait_text(m, 'cmdline: abcdef')
    m.press('down', 'down', 'end', 'up')
    m.type_text('X')
    m.press('home', *['right'] * len('cmdline: '), 'delete', 'right', 'backspace')
    m.type_text('Y')
    m.press('end', 'left', 'left')
    m.type_text('Z')
    t.wait_text(m, 'cmdline: YcdeZfX')
    m.press(('ctrl', 'x'))
    t.expect_eq(booted_cmdline(t, m), 'YcdeZfX', 'booted as edited')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_editor_discard(t):
    """ESC leaves the editor for the menu, the edit discarded [intent, from
    the help]."""
    m = boot_menu(t, 'timeout: no\n' + menu_entry(1, 'Only', 'original'))
    wait_menu(t, m)
    m.press('e')
    t.wait_text(m, 'cmdline: original')
    m.press('down', 'down', 'end')
    m.type_text(' edited')
    t.wait_text(m, 'cmdline: original edited')
    m.press('esc')
    wait_menu(t, m)
    m.press('ret')
    t.expect_eq(booted_cmdline(t, m), 'original', 'the entry as configured booted')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_editor_disabled(t):
    """editor_enabled: no takes the editor away."""
    m = boot_menu(t, 'timeout: no\neditor_enabled: no\n' + menu_entry(1, 'Only', 'original'))
    scr = wait_menu(t, m)
    t.check('E Edit' not in screen_text(scr), '[intent] no edit key in the help')
    m.press('e')
    time.sleep(1.5)
    t.check('cmdline: original' not in screen_text(t.screen(m)), 'E does not open the editor')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_keyboard_layout(t):
    """keyboard_layout remaps what the firmware reads as US QWERTY keys
    before the menu and the editor see it: on Dvorak the key in QWERTY's D
    place is E, the edit key. An unknown layout remaps nothing."""
    for layout, edit, typed, expect in (('dvorak', 'd', 'jdpps', 'hello'), ('azerty', 'e', 'qwaz;', 'azqwm'),
                                        ('colemak', 'e', 'jdpps', 'jdpps')):
        m = boot_menu(t, f'timeout: no\nkeyboard_layout: {layout}\n' + menu_entry(1, 'Only', 'x'))
        wait_menu(t, m)
        m.press(edit)
        t.wait_text(m, 'cmdline: x')
        m.press('down', 'down', 'end')
        m.type_text(typed)
        t.wait_text(m, f'cmdline: x{expect}')
        m.stop()


@check(ports=UEFI_PORTS, tags=['menu'])
def menu_remember_last_entry(t):
    """remember_last_entry: yes makes the entry booted last the one selected
    on the next boot, ahead of default_entry."""
    config = tree_menu('timeout: 3\nremember_last_entry: yes\ndefault_entry: 1\n')
    tree = Tree()
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
    t.limine_files(config, tree)
    disk = t.boot_disk(tree)
    m = t.boot(disk)
    t.wait_text(m, 'Booting automatically')
    m.press('down', 'down', 'down', 'down', 'down', 'ret')
    t.expect_eq(booted_cmdline(t, m), 'last', 'Last chosen')
    m.stop()
    stored = efivars.read(m.workdir + '/vars.fd')
    t.check(('LimineLastBootedEntry', LIMINE_VENDOR) in stored,
            f'[intent] remembered under Limine\'s vendor GUID: {sorted(n for n, _ in stored)}')
    m = t.boot(disk, vars_file=m.workdir + '/vars.fd')
    t.expect_eq(booted_cmdline(t, m), 'last', '[spec] Last selected and booted when the timeout ran out')

