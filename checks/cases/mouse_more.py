import time

from harness.api import check
from harness.media import HostFile, Tree
from harness.ports import ALL_PORTS, UEFI_PORTS, Drive
from harness.screen import Font, flanterm_geometry
from cases._common import kernel_file
from cases.menu import booted_cmdline, tree_menu
from cases.mouse import ABS_MAX, Pointer, selected

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


@check(ports=UEFI_PORTS, tags=['mouse'])
def mouse_rotated_absolute(t):
    """On a rotated interface an absolute pointer's position is one on the
    screen as the firmware drives it, as a tablet's is, and not on the
    interface drawn turned: a click where an entry shows boots it [intent]."""
    font = Font()
    for degrees in (90, 180, 270):
        m = boot_with_mouse(t, f'interface_rotation: {degrees}\n' + tree_menu(), 'virtio-tablet-pci')
        scr = t.wait_text(m, 'ARROWS', rotate=360 - degrees)
        img = m.screendump()
        hits = scr.find(' Last ')
        t.require(hits, f'{degrees}: Last on screen')
        col, row = hits[0][0] + 1 + len('Last') // 2, hits[0][1]
        # The middle of that cell on the interface, then where Flanterm puts
        # it on the screen.
        width, height = (img.width, img.height) if degrees == 180 else (img.height, img.width)
        _, _, off_x, off_y = flanterm_geometry(width, height, font)
        x, y = off_x + col * font.cell_w + font.cell_w // 2, off_y + row * font.cell_h + font.cell_h // 2
        px, py = {90: (height - 1 - y, x), 180: (width - 1 - x, height - 1 - y), 270: (y, width - 1 - x)}[degrees]
        m.mouse_abs(int((px + 0.5) * ABS_MAX / img.width), int((py + 0.5) * ABS_MAX / img.height))
        time.sleep(0.15)
        m.mouse_click()
        t.expect_eq(booted_cmdline(t, m), 'last', f'{degrees}: a click where Last shows booted it')
        m.stop()
