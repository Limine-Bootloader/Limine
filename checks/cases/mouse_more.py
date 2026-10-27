import time

from harness.api import check
from harness.media import HostFile, Tree
from harness.ports import ALL_PORTS, Drive
from cases._common import kernel_file
from cases.menu import booted_cmdline, tree_menu
from cases.mouse import Pointer, selected

# The menu's pointer beyond what mouse.py covers: the pointer in VGA text
# mode, and on a rotated interface.


def boot_with_mouse(t, config, mouse):
    tree = Tree()
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
    t.limine_files(config, tree)
    return t.boot(drives=[Drive(t.boot_disk(tree).path)], mouse=mouse)


@check(ports=['bios'], tags=['mouse'])
def mouse_textmode(t):
    """In VGA text mode the pointer is the text cursor, and clicks work the
    same [intent]."""
    m = boot_with_mouse(t, 'graphics: no\n' + tree_menu(), 'ps2')
    scr = t.wait_text(m, 'ARROWS')
    p = Pointer(t, m, scr)
    p.park(p.point_at(scr, 'Last'))
    t.wait_screen(m, lambda s: selected(s, 'Last'), desc='Last selected')
    p.point_at(scr, 'Last')
    m.mouse_click()
    t.expect_eq(booted_cmdline(t, m), 'last', 'a click booted Last')


@check(ports=ALL_PORTS, tags=['mouse'])
def mouse_rotated(t):
    """On a rotated interface the pointer is drawn turned with it; moving it,
    the wheel and the right button change nothing a key cannot set back."""
    for degrees in (90, 180, 270):
        mouse = 'ps2' if not t.port.is_uefi else 'virtio-tablet-pci'
        m = boot_with_mouse(t, f'interface_rotation: {degrees}\n' + tree_menu(), mouse)
        t.wait_text(m, 'ARROWS', rotate=360 - degrees)
        if t.port.is_uefi:
            for x, y in ((0x1000, 0x1000), (0x4000, 0x6000), (0x7000, 0x2000)):
                m.mouse_abs(x, y)
                time.sleep(0.15)
        else:
            for dx, dy in ((100, 50), (-30, 120), (60, -60)):
                m.mouse_move(dx, dy)
                time.sleep(0.05)
        m.mouse_wheel(1)
        m.mouse_click('right')
        time.sleep(0.3)
        m.press('home', 'ret')
        t.expect_eq(booted_cmdline(t, m), 'first', f'{degrees}: Home and Enter still boot the first entry')
        m.stop()
