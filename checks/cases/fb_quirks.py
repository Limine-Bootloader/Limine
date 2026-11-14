import re
import struct
import time

from harness.api import check
from harness.media import HostFile
from harness.ports import ALL_PORTS, UEFI_PORTS
from harness import screen as screen_mod
from cases._common import boot_entry, check_report_clean, entry, kernel_file, screen_text
from cases.chainload import probe_then_limine
from cases.multiboot import mb1_kernel, mb1_report, mb2_kernel, mb2_report

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


# A portrait panel whose modes GOP reports landscape.
MIIX_320 = [smbios(1, manufacturer='LENOVO', version='Lenovo MIIX 320-10ICR')]
# The panel's right edge at the top of the casing, at 800x1280; GOP's 1280x800
# mode turned in software.
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
    """The Limine protocol test kernel's report, booted on the probe's GOPs
    on UEFI, on VBE on BIOS."""
    config = 'timeout: 0\n' + globals_ + entry('Test', path='boot():/boot/kernel.elf', cmdline='x', extra=extra)
    if t.port.is_uefi:
        m = boot_fake(t, smbios_, config, {'/boot/kernel.elf': HostFile(kernel_file(t, 'full'))})
    else:
        m, _ = boot_entry(t, config, smbios=smbios_)
    r = t.kernel_report(m)
    check_report_clean(t, r)
    m.stop()
    return r


def geometry(fb):
    """A framebuffer or mode as the test kernel reports it, but for its
    memory model and masks."""
    return ' '.join(fb.split()[:2])


def upright(r):
    return r.int('framebuffer.0.upright_rotation')


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
    t.expect_eq(r.int('framebuffer.1.upright_rotation'), ROTATE_0, 'no turn given for it')

    r = report(MIIX_320)
    width, height = (int(v) for v in r['framebuffer.0'].split('x')[:2])
    t.check(width < height, f'a portrait panel\'s: the firmware\'s framebuffer exchanged: {r["framebuffer.0"]}')
    t.expect_eq(geometry(r['framebuffer.1']), '800x600x32 pitch=3200', 'the probe\'s as reported')
    t.expect_eq(r.indexed('framebuffer.1.mode'),
                ['640x480x16 pitch=1280', '800x600x32 pitch=3200', '320x200x16 pitch=640',
                 '800x1280x32 pitch=3200', '720x1280x32 pitch=2880', '1280x800x32 pitch=5376'],
                'and its modes')


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


@check(ports=UEFI_PORTS, tags=['fb-quirks', 'limine-protocol', 'gop'])
def fb_quirks_upright_rotation(t):
    """The Limine protocol's upright_rotation gives the clockwise turn that
    rights what is drawn on a panel mounted turned in its casing, whatever
    interface_rotation says, and 0 where none is known."""
    portrait = 'resolution: 800x1280x32'
    t.expect_eq(upright(protocol_report(t, STEAM_DECK, portrait)), ROTATE_90, 'right edge up')
    t.expect_eq(upright(protocol_report(t, DYNABOOK_K50, portrait)), ROTATE_270, 'left edge up')
    t.expect_eq(upright(protocol_report(t, STEAM_DECK)), ROTATE_0, 'a mode not the panel\'s size')
    for degrees in (0, 180):
        r = protocol_report(t, STEAM_DECK, portrait, globals_=f'interface_rotation: {degrees}\n')
        t.expect_eq(upright(r), ROTATE_90, f'right edge up, interface_rotation: {degrees}')


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


@check(ports=ALL_PORTS, tags=['fb-quirks', 'limine-protocol', 'gop'])
def fb_quirks_portrait_panels(t):
    """On a machine SMBIOS names as one whose GOP reports its portrait
    panel's modes with width and height exchanged, the kernel gets the
    current mode and every other the right way round, their rows taken to be
    unpadded; a mode reported portrait is taken to be right already
    [intent]. VBE's modes are taken as reported."""
    if not t.port.is_uefi:
        plain = protocol_report(t, [])
        r = protocol_report(t, MIIX_320)
        t.expect_eq((r['framebuffer.0'], r.indexed('framebuffer.0.mode')),
                    (plain['framebuffer.0'], plain.indexed('framebuffer.0.mode')), 'VBE\'s modes as reported')
        return
    r = protocol_report(t, MIIX_320)
    t.expect_eq(geometry(r['framebuffer.0']), '600x800x32 pitch=2400', 'the EDID\'s 800x600')
    t.expect_eq(r.indexed('framebuffer.0.mode'),
                ['480x640x16 pitch=960', '600x800x32 pitch=2400', '200x320x16 pitch=400',
                 '800x1280x32 pitch=3200', '720x1280x32 pitch=2880', '800x1280x32 pitch=3200'],
                'the modes, those reported landscape exchanged')
    r = protocol_report(t, MIIX_320, 'resolution: 1280x800x32')
    t.expect_eq(geometry(r['framebuffer.0']), '800x1280x32 pitch=3200', '1280x800 in padded rows')


def structure(type_, handle, formatted, strings):
    """An SMBIOS structure (DSP0134 3.9.0 section 6.1.2): its header, the
    rest of its formatted area, and its strings."""
    data = struct.pack('<BBH', type_, 4 + len(formatted), handle) + formatted
    return data + b''.join(s.encode() + b'\0' for s in strings) + (b'\0' if strings else b'\0\0')


@check(ports=UEFI_PORTS, tags=['fb-quirks', 'limine-protocol', 'gop'])
def fb_quirks_keys(t):
    """A machine is named by SMBIOS strings that each equal, or hold, a text
    byte for byte, case included, all of an entry's matching. A string comes
    from the first structure of its type long enough to give its number."""
    def exchanged(smbios_):
        return geometry(protocol_report(t, smbios_)['framebuffer.0']) == '600x800x32 pitch=2400'

    miix_310 = smbios(1, manufacturer='LENOVO', version='MIIX 310-10ICR')
    t.check(exchanged([miix_310, smbios(0, version='1HCN44WW')]), 'Miix 310-10ICR and its BIOS version')
    t.check(not exchanged([miix_310]), 'Miix 310-10ICR without its BIOS version')
    t.check(exchanged([smbios(1, manufacturer='LENOVO', version='IdeaPad Duet 3 10IGL5-LTE')]),
            'a version holding the text')
    t.check(not exchanged([smbios(1, manufacturer='Lenovo', version='Lenovo MIIX 320-10ICR')]),
            'the manufacturer in other case')
    blobs = [structure(1, 0x0e00, bytes((1, 2)), ['LENOVO', '80XF']),
             structure(1, 0x0e01, bytes((1, 2, 3, 0)) + bytes(16) + bytes((6, 0, 0)),
                       ['Someone Else', 'Something', 'Lenovo MIIX 320-10ICR'])]
    files = []
    for i, blob in enumerate(blobs):
        path = t.path(f'type1-{i}.bin')
        with open(path, 'wb') as f:
            f.write(blob)
        files.append(f'file={path}')
    t.check(exchanged(files), 'the manufacturer from a structure too short to hold the version, the version from '
                              'the next')


@check(ports=UEFI_PORTS, tags=['fb-quirks', 'limine-protocol', 'gop'])
def fb_quirks_fixed_mode(t):
    """On a Steam Deck, whose firmware turns its 1280x800 mode in software
    while memory keeps the panel's own layout, 800x1280 in rows of 832
    pixels, that mode is given as laid out, and only that one; the panel's
    turn is then known at its own size."""
    r = protocol_report(t, STEAM_DECK, 'resolution: 1280x800x32')
    t.expect_eq(geometry(r['framebuffer.0']), '800x1280x32 pitch=3328', '1280x800')
    t.expect_eq(r.indexed('framebuffer.0.mode'),
                ['640x480x16 pitch=1280', '800x600x32 pitch=3200', '320x200x16 pitch=640',
                 '800x1280x32 pitch=3200', '720x1280x32 pitch=2880', '800x1280x32 pitch=3328'],
                'the modes, 1280x800 alone given as laid out')
    t.expect_eq(upright(r), ROTATE_90, 'the turn of the panel at its own size')
    r = protocol_report(t, STEAM_DECK)
    t.expect_eq(geometry(r['framebuffer.0']), '800x600x32 pitch=3200', 'another mode as reported')


@check(ports=UEFI_PORTS, tags=['fb-quirks', 'gop', 'gterm'])
def fb_quirks_portrait_interface(t):
    """Limine's interface is drawn on a portrait panel whose modes GOP
    reports landscape as it is laid out."""
    config = 'timeout: no\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x')
    m = boot_fake(t, MIIX_320, config, {'/boot/kernel.elf': HostFile(kernel_file(t, 'full'))})
    wait_fake_text(t, m, 'ARROWS', 600, 800, 2400)


@check(ports=['uefi-ia32', 'uefi-x86-64'], tags=['fb-quirks', 'multiboot1', 'gop'])
def fb_quirks_portrait_after_boot_services(t):
    """The terminal Limine keeps for panicking once boot services are gone
    draws on a portrait panel whose modes GOP reports landscape as it is
    laid out, whatever the kernel is given [intent]. Here it shows
    Multiboot 1's panic at a memory map the probe fragmented past what Limine
    passes."""
    config = 'timeout: 0\n' + entry('Test', protocol='multiboot1', path='boot():/kernel', cmdline='x')
    m = probe_then_limine(t, 'fakegop:2 fragment:180', config, {'/kernel': HostFile(mb1_kernel(t, 'video'))},
                          display=False, smbios=MIIX_320)
    wait_fake_text(t, m, 'Too many memory map entries', 600, 800, 2400)


@check(ports=UEFI_PORTS, tags=['fb-quirks', 'limine-protocol', 'linux', 'multiboot1', 'multiboot2', 'gop'])
def fb_quirks_for_limine_only(t):
    """The firmware's own description of a framebuffer is corrected for the
    Limine protocol's kernels alone. Linux corrects it itself, and is given
    it as the firmware reports it, as are Multiboot 1 and 2 kernels
    [intent]."""
    from cases.linux import bzimage, hexint, stub, stub_report, x86_report
    t.expect_eq(geometry(protocol_report(t, MIIX_320)['framebuffer.0']), '600x800x32 pitch=2400',
                'the Limine protocol: the mode corrected')
    if t.port.is_x86:
        path, image, report, prefix = '/bzImage', bzimage(t), x86_report, 'si.'
    else:
        path, image, report, prefix = '/Image', stub(t), stub_report, 'screen_info.'
    config = 'timeout: 0\n' + entry('Linux', protocol='linux', path=f'boot():{path}', cmdline='x')
    m = boot_fake(t, MIIX_320, config, {path: image})
    r = report(t, m)
    t.expect_eq(tuple(hexint(r, prefix + k) for k in ('width', 'height', 'linelength')), (800, 600, 3200),
                'Linux: the mode as reported')
    m.stop()
    if not t.port.is_x86:
        return
    for protocol, kernel, mb_report in (('multiboot1', mb1_kernel(t, 'video'), mb1_report),
                                        ('multiboot2', mb2_kernel(t, 'fb'), mb2_report)):
        config = 'timeout: 0\n' + entry('MB', protocol=protocol, path='boot():/kernel', cmdline='x')
        m = boot_fake(t, MIIX_320, config, {'/kernel': HostFile(kernel)})
        r = mb_report(t, m)
        t.expect_eq(tuple(hexint(r, 'fb.' + k) for k in ('width', 'height', 'pitch')), (800, 600, 3200),
                    f'{protocol}: the mode as reported')
        m.stop()
