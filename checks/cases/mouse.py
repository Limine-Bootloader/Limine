import time

from harness.api import check
from harness.media import HostFile, Tree
from harness.ports import ALL_PORTS, Drive
from cases._common import countdown_stopped, kernel_file, screen_text
from cases.menu import EDITOR_ENTRY, booted_cmdline, nothing_booted, tree_menu, wait_menu

# The mouse in the menu, as CONFIG.md's mouse option describes it: on BIOS a
# PS/2 mouse, on UEFI a pointer the firmware exposes: a virtio tablet, an
# absolute pointer, or a USB mouse, a relative one (the firmware's
# drivers; edk2 firmware exposes no pointer for QEMU's USB tablet).

ABS_MAX = 0x7fff
# Motion counts per column and per row [intent]: Limine takes 8 PS/2 counts
# a column and 16 a row, and a cell as 2mm by 4mm of a simple pointer's
# motion; the edk2 firmware gives a USB mouse 8 counts a millimetre.
COUNTS = {'ps2': (8, 16), 'usb-mouse': (16, 32)}


def default_mouse(t):
    return 'ps2' if not t.port.is_uefi else 'virtio-tablet-pci'


def mouse_boot(t, config, mouse=None):
    tree = Tree()
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
    t.limine_files(config, tree)
    # No boot order: given one, RISC-V's edk2 connects only the devices it
    # lists, and the pointer stays without a driver.
    return t.boot(drives=[Drive(t.boot_disk(tree).path)], mouse=mouse or default_mouse(t))


def ps2_move(m, dx, dy):
    """Relative motion in steps one PS/2 packet (or USB report) carries,
    paced: QEMU holds back what its queue has no room for and folds it into
    the next motion."""
    while dx or dy:
        sx, sy = max(-200, min(200, dx)), max(-200, min(200, dy))
        m.mouse_move(sx, sy)
        dx, dy = dx - sx, dy - sy
        time.sleep(0.05)


class Pointer:
    """Puts the pointer in the middle of a cell: a tablet by its absolute
    position, a mouse by motion from the top left corner, in COUNTS."""

    def __init__(self, t, m, scr, mouse=None):
        self.t, self.m, self.cols, self.rows = t, m, scr.cols, scr.rows
        self.mouse = mouse or default_mouse(t)
        self.counts = None

    def move(self, x, y):
        if self.mouse not in COUNTS:
            self.m.mouse_abs(int((x + 0.5) * ABS_MAX / self.cols), int((y + 0.5) * ABS_MAX / self.rows))
            # Long enough for the firmware to report the move alone, not as
            # one state with a press that follows, which Limine drops if it
            # has not yet seen the button up.
            time.sleep(0.15)
            return
        per_col, per_row = COUNTS[self.mouse]
        if self.counts is None:
            ps2_move(self.m, -self.cols * per_col, -self.rows * per_row)
            self.counts = (0, 0)
        cx, cy = x * per_col + per_col // 2, y * per_row + per_row // 2
        ps2_move(self.m, cx - self.counts[0], cy - self.counts[1])
        self.counts = (cx, cy)
        if self.t.port.is_uefi:
            time.sleep(0.15)

    def point_at(self, scr, name):
        """Onto the middle of an entry's name; returns its row."""
        hits = scr.find(f' {name} ')
        self.t.require(hits, f'{name!r} on screen')
        x, y = hits[0]
        self.move(x + 1 + len(name) // 2, y)
        return y

    def park(self, y):
        """To the end of row y: the pointer hides what it is drawn over, and
        moving along a row keeps the entry on it selected."""
        self.move(self.cols - 1, y)


def selected(scr, name):
    """Whether an entry is shown selected: in reverse video [intent], its name
    on other than the screen's background."""
    hits = scr.find(f' {name} ')
    if not hits:
        return False
    x, y = hits[0]
    return scr.cell(x + 1, y).bg != scr.cell(0, y).bg


@check(ports=ALL_PORTS, tags=['mouse'])
def mouse_point_and_click(t):
    """The selection follows the pointer, and a left click boots the entry
    under it; on UEFI with an absolute pointer and with a relative one."""
    for mouse in ('virtio-tablet-pci', 'usb-mouse') if t.port.is_uefi else ('ps2',):
        m = mouse_boot(t, tree_menu(), mouse)
        scr = t.wait_text(m, 'ARROWS')
        t.check(selected(scr, 'First') and not selected(scr, 'Last'), f'{mouse}: First selected to begin with')
        p = Pointer(t, m, scr, mouse)
        p.park(p.point_at(scr, 'Last'))
        t.wait_screen(m, lambda s: selected(s, 'Last'), desc=f'{mouse}: Last selected')
        p.point_at(scr, 'Last')
        m.mouse_click()
        t.expect_eq(booted_cmdline(t, m), 'last', f'{mouse}: a click booted Last')
        m.stop()


@check(ports=ALL_PORTS, tags=['mouse'])
def mouse_click_directory(t):
    """A click on a directory expands it."""
    m = mouse_boot(t, tree_menu())
    scr = t.wait_text(m, 'ARROWS')
    p = Pointer(t, m, scr)
    y = p.point_at(scr, '[+] Collapsed')
    t.wait_screen(m, lambda s: 'ENTER Expand' in screen_text(s), desc='on the directory')
    m.mouse_click()
    p.park(y)
    t.wait_text(m, 'Hidden Child')


# With edk2 firmware a virtio tablet's wheel never reaches the pointer; a USB
# mouse's does.
@check(ports=ALL_PORTS, tags=['mouse'])
def mouse_wheel(t):
    """The wheel moves the selection, one entry a notch, down when rolled
    towards the user: on UEFI the USB mouse's Wheel, whose values grow
    rolled away from the user [spec: HID Usage Tables 1.7, 4.3]. The first
    notch counts, once the menu is up as once back from the editor [user]."""
    m = mouse_boot(t, tree_menu(), 'usb-mouse' if t.port.is_uefi else 'ps2')
    t.wait_text(m, 'the first comment')
    m.mouse_wheel(2)
    t.wait_text(m, 'an inner comment')
    m.mouse_wheel(-2)
    t.wait_text(m, 'the first comment')
    m.press('e')
    t.wait_text(m, 'cmdline: first')
    m.press('esc')
    wait_menu(t, m)
    m.mouse_wheel(2)
    t.wait_text(m, 'an inner comment')


@check(ports=ALL_PORTS, tags=['mouse'])
def mouse_stops_countdown(t):
    """Using the mouse stops the countdown, as a key does."""
    # Long enough for the pointer to move while it runs, however slow the
    # machine.
    m = mouse_boot(t, tree_menu('timeout: 30\n'))
    scr = t.wait_text(m, 'Booting automatically')
    Pointer(t, m, scr).point_at(scr, 'Last')
    t.wait_screen(m, lambda s: 'Booting automatically' not in screen_text(s), desc='countdown gone')
    t.check(countdown_stopped(t, m) and nothing_booted(t, m, 0), 'the countdown stopped')


@check(ports=ALL_PORTS, tags=['mouse'])
def mouse_disabled(t):
    """mouse: no leaves the pointer out of the menu."""
    m = mouse_boot(t, tree_menu('timeout: no\nmouse: no\n'))
    scr = t.wait_text(m, 'the first comment')
    Pointer(t, m, scr).point_at(scr, 'Last')
    m.mouse_wheel(2)
    m.mouse_click()
    t.check(nothing_booted(t, m, 3), 'a click booted nothing')
    t.check('the first comment' in screen_text(t.screen(m)), 'First still selected')
    m.press('ret')
    t.expect_eq(booted_cmdline(t, m), 'first', 'the keyboard still works')


@check(ports=ALL_PORTS, tags=['mouse'])
def mouse_editor(t):
    """In the editor, a click puts the cursor on the character clicked, or
    at the end of the line when past it [intent], and the wheel moves it up
    a line a notch; on UEFI with the USB mouse, whose wheel edk2 passes on."""
    mouse = 'usb-mouse' if t.port.is_uefi else 'ps2'
    m = mouse_boot(t, 'timeout: no\n' + EDITOR_ENTRY, mouse)
    t.wait_text(m, 'ARROWS')
    m.press('e')
    scr = t.wait_text(m, 'cmdline: abcdef')
    x, y = scr.find('cmdline: abcdef')[0]
    p = Pointer(t, m, scr, mouse)
    # edk2 passes on what a pointer does at its next poll: a key typed
    # straight after a click could overtake it.
    p.move(scr.cols - 1, y)
    m.mouse_click()
    time.sleep(0.3)
    m.type_text('E')
    p.move(x + len('cmdline: ab'), y)
    m.mouse_click()
    time.sleep(0.3)
    m.type_text('Q')
    m.press('down')
    m.mouse_wheel(-1)
    time.sleep(0.3)
    m.type_text('W')
    m.press('f10')
    t.expect_eq(booted_cmdline(t, m), 'abQWcdefE', 'edited where clicked')
