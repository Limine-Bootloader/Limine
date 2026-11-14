import re
import time

from harness.api import check
from harness.media import HostFile
from harness.ports import UEFI_PORTS
from harness import screen as screen_mod
from cases._common import check_report_clean, entry, kernel_file, screen_text
from cases.chainload import probe_then_limine
from cases.multiboot import mb1_kernel

# What Limine knows of machines' framebuffers beyond what their firmware says,
# by the SMBIOS strings common/lib/fb_quirks.c lists the machines by, as
# CONFIG.md and the Limine protocol's specification say it is used. The
# machines are QEMU ones given those strings, their framebuffers the EFI
# probe's GOPs (payloads/efi/efiprobe.c) alone, in its portrait modes where a
# panel's size is wanted.

ROTATE_0, ROTATE_90, ROTATE_180, ROTATE_270 = range(4)


def smbios(type_, **fields):
    """A QEMU -smbios option giving fields of the structure of type type_,
    with each comma in a value doubled as QEMU takes it."""
    return f'type={type_}' + ''.join(f',{k}={v.replace(",", ",,")}' for k, v in fields.items())


# The panel's right edge at the top of the casing, at 800x1280.
STEAM_DECK = [smbios(1, manufacturer='Valve', product='Jupiter', version='1')]
# Its left edge.
DYNABOOK_K50 = [smbios(1, manufacturer='Dynabook Inc.', product='dynabook K50/FR')]


def default_strings(date):
    """The strings of firmware that left them at their defaults, as GPD's
    early firmware did, its machines told apart by BIOS date and panel
    size."""
    return [smbios(0, date=date), smbios(1, manufacturer='Default string', product='Default string'),
            smbios(2, manufacturer='Default string', product='Default string')]


def boot_fake(t, smbios_, config, files):
    return probe_then_limine(t, 'fakegop:2', config, files, display=False, smbios=smbios_)


def protocol_report(t, smbios_, extra='', globals_=''):
    """The Limine protocol test kernel's report, booted on the probe's GOPs."""
    config = 'timeout: 0\n' + globals_ + entry('Test', path='boot():/boot/kernel.elf', cmdline='x', extra=extra)
    m = boot_fake(t, smbios_, config, {'/boot/kernel.elf': HostFile(kernel_file(t, 'full'))})
    r = t.kernel_report(m)
    check_report_clean(t, r)
    m.stop()
    return r


def interface_rotation(r):
    """The rotation Limine's terminal had, as the parameters given to the
    kernel to set one up alike say."""
    return int(re.search(r'rotation=(\d+)', r['flanterm.0.font']).group(1))


def fake_fb_address(t, m):
    """Where the probe put its GOPs' framebuffer, once it says so."""
    found = m.wait_for(t.kernel_channel, re.compile(rb'K fakegop\.fb=([0-9a-fA-F]+)\r?\n'))
    return int(found.group(1), 16)


def fake_screen(m, address, width, height, pitch, rotate):
    """The text on the probe's framebuffer, in a mode of 32-bit pixels whose
    colour channels are read in whatever order, turned rotate degrees
    clockwise first."""
    raw = m.read_phys(address, pitch * height)
    rows = b''.join(raw[y * pitch:y * pitch + width * 4] for y in range(height))
    rgb = bytearray(width * height * 3)
    for channel in range(3):
        rgb[channel::3] = rows[channel::4]
    return screen_mod.decode_gterm(screen_mod.Image(width, height, bytes(rgb)).rotate(rotate))


def wait_fake_text(t, m, needle, width, height, pitch, rotate=0, timeout=30):
    """Poll the probe's framebuffer until needle is on it."""
    address = fake_fb_address(t, m)
    deadline = time.monotonic() + timeout * t.port.timeout_scale
    while True:
        scr = fake_screen(m, address, width, height, pitch, rotate)
        if scr.find(needle):
            return scr
        if m.exited() or time.monotonic() > deadline:
            t.fail(f'no {needle!r} on the {width}x{height} framebuffer turned {rotate} degrees:\n'
                   + screen_text(scr))
        time.sleep(0.5)


@check(ports=UEFI_PORTS, tags=['fb-quirks', 'limine-protocol', 'gop', 'gterm'])
def fb_quirks_first_framebuffer_only(t):
    """Limine goes by what SMBIOS names the machine as for the first
    framebuffer alone, the one the Linux protocol hands Linux [intent]. With
    QEMU's display present that is the firmware's GOP's, and the probe's,
    the second, is left as reported, Limine's interface on it unturned."""
    def report(smbios_, extra=''):
        config = 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x', extra=extra)
        m = probe_then_limine(t, 'fakegop:2', config, {'/boot/kernel.elf': HostFile(kernel_file(t, 'full'))},
                              smbios=smbios_)
        r = t.kernel_report(m)
        check_report_clean(t, r)
        m.stop()
        return r

    r = report(STEAM_DECK, 'resolution: 800x1280x32')
    t.expect_eq(r['framebuffer.1'].split()[0], '800x1280x32', 'the probe\'s framebuffer, of the panel\'s size')
    rotation = int(re.search(r'rotation=(\d+)', r['flanterm.1.font']).group(1))
    t.expect_eq(rotation, ROTATE_0, 'the interface on it unturned')


@check(ports=UEFI_PORTS, tags=['fb-quirks', 'limine-protocol', 'gop', 'gterm'])
def fb_quirks_turned_panels(t):
    """Without interface_rotation, Limine's interface is turned to look
    upright on a framebuffer of the size of a panel SMBIOS names as mounted
    turned in its casing (CONFIG.md), 90 degrees for the panel's right edge at
    the top of the casing and 270 for its left; any other framebuffer is left
    unturned. The first entry matching the machine, the size, and any BIOS
    date it lists decides, those failing on size or date passed over."""
    portrait = 'resolution: 800x1280x32'
    t.expect_eq(interface_rotation(protocol_report(t, STEAM_DECK, portrait)), ROTATE_90, 'right edge up')
    t.expect_eq(interface_rotation(protocol_report(t, STEAM_DECK)), ROTATE_0,
                'right edge up, a mode not the panel\'s size')
    t.expect_eq(interface_rotation(protocol_report(t, DYNABOOK_K50, portrait)), ROTATE_270, 'left edge up')
    gpd = 'resolution: 720x1280x32'
    t.expect_eq(interface_rotation(protocol_report(t, default_strings('04/26/2019'), gpd)), ROTATE_90,
                'default strings and the first date listed for them')
    t.expect_eq(interface_rotation(protocol_report(t, default_strings('05/24/2018'), gpd)), ROTATE_90,
                'default strings and a date listed after an entry of the wrong date and one of the wrong size')
    t.expect_eq(interface_rotation(protocol_report(t, default_strings('01/01/2000'), gpd)), ROTATE_0,
                'default strings and no date listed')
    gpd_win = [smbios(0, date='02/21/2017'), smbios(1, product='Default string'),
               smbios(2, manufacturer='AMI Corporation', product='Default string', serial='Default string')]
    t.expect_eq(interface_rotation(protocol_report(t, gpd_win, gpd)), ROTATE_90,
                'a key on the board\'s serial number')


@check(ports=UEFI_PORTS, tags=['fb-quirks', 'limine-protocol', 'gop', 'gterm'])
def fb_quirks_interface_rotation(t):
    """interface_rotation, 0 included, overrides the turn a panel mounted
    turned asks for (CONFIG.md); the menu is drawn turned or not as the
    terminal parameters given to the kernel say."""
    portrait = 'resolution: 800x1280x32'
    r = protocol_report(t, STEAM_DECK, portrait, globals_='interface_rotation: 0\n')
    t.expect_eq(interface_rotation(r), ROTATE_0, 'interface_rotation: 0')
    r = protocol_report(t, STEAM_DECK, portrait, globals_='interface_rotation: 180\n')
    t.expect_eq(interface_rotation(r), ROTATE_180, 'interface_rotation: 180')

    menu = 'timeout: no\ninterface_resolution: 800x1280\n'
    config = menu + entry('Test', path='boot():/boot/kernel.elf', cmdline='x')
    m = boot_fake(t, STEAM_DECK, config, {'/boot/kernel.elf': HostFile(kernel_file(t, 'full'))})
    wait_fake_text(t, m, 'ARROWS', 800, 1280, 3200, rotate=270)
    m.stop()
    m = boot_fake(t, STEAM_DECK, 'interface_rotation: 0\n' + config,
                  {'/boot/kernel.elf': HostFile(kernel_file(t, 'full'))})
    wait_fake_text(t, m, 'ARROWS', 800, 1280, 3200)
    m.stop()


@check(ports=['uefi-ia32', 'uefi-x86-64'], tags=['fb-quirks', 'multiboot1', 'gop'])
def fb_quirks_rotation_after_boot_services(t):
    """The terminal Limine keeps for panicking once boot services are gone is
    turned upright on a panel mounted turned [intent]. Here it shows
    Multiboot 1's panic at a memory map the probe fragmented past what Limine
    passes."""
    config = 'timeout: 0\n' + entry('Test', protocol='multiboot1', path='boot():/kernel', cmdline='x',
                                    extra='resolution: 800x1280x32')
    m = probe_then_limine(t, 'fakegop:2 fragment:180', config, {'/kernel': HostFile(mb1_kernel(t, 'video'))},
                          display=False, smbios=STEAM_DECK)
    wait_fake_text(t, m, 'Too many memory map entries', 800, 1280, 3200, rotate=270)
