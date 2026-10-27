from harness.api import check
from harness.media import HostFile
from harness.ports import UEFI_PORTS
from harness.report import fnv1a
from cases._common import check_report_clean, entry, kernel_file
from cases.chainload import probe_then_limine

# Graphics output the way QEMU's display devices never offer it, through
# GOPs the EFI probe installs over plain memory before it starts Limine: in
# the probe's modes (payloads/efi/efiprobe.c), 640x480 RGB 5:6:5 by bit masks,
# 800x600 8-bit RGB, others blank-masked, blit-only, or with a pitch below
# their width, and an EDID that prefers 800x600. Two such GOPs share their
# memory.


def boot_fake_gop(t, extra=''):
    m = probe_then_limine(t, 'name:fakegop fakegop:2',
                          'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x', extra=extra),
                          {'/boot/kernel.elf': HostFile(kernel_file(t, 'full'))})
    r = t.kernel_report(m)
    check_report_clean(t, r)
    m.stop()
    return r


def fake_edid():
    """The probe's EDID: a header, version 1.3, and a first detailed timing
    of 800x600."""
    edid = bytearray(128)
    edid[0:8] = b'\0\xff\xff\xff\xff\xff\xff\0'
    edid[18:20] = b'\x01\x03'
    edid[54:62] = bytes((0x64, 0x19, 0x20, 0x00, 0x30, 0x58, 0x00, 0x20))
    edid[127] = -sum(edid) & 0xff
    return bytes(edid)


def fake_framebuffer(t, r):
    sizes, hashes = r.multi.get('framebuffer.edid_size', []), r.multi.get('framebuffer.edid_hash', [])
    t.check(len(sizes) == 2 and sizes[1] == '128' and fnv1a(fake_edid()) in hashes,
            f'[spec] the fake framebuffer\'s EDID passed on: sizes {sizes}')
    t.expect_eq(r.int('framebuffer.count'), 2, "the firmware's framebuffer and one fake: the second fake, "
                                               'over the same memory, left out')
    return r['framebuffer.1']


@check(ports=UEFI_PORTS, tags=['limine-protocol', 'gop'])
def gop_pixel_formats(t):
    """Without a resolution asked for, the EDID's preferred one is taken; a
    resolution names the mode, its pixel format becoming the framebuffer's
    masks (UEFI 2.11 12.9: 8-bit RGB has red in the lowest byte, a bit mask
    format its masks); modes without a usable format or pitch are passed
    over [spec]."""
    fb = fake_framebuffer(t, boot_fake_gop(t))
    t.expect_eq(fb, '800x600x32 pitch=3200 model=1 r=8:0 g=8:8 b=8:16', 'the EDID mode, 8-bit RGB')
    fb = fake_framebuffer(t, boot_fake_gop(t, 'resolution: 640x480x16'))
    t.expect_eq(fb, '640x480x16 pitch=1280 model=1 r=5:11 g=6:5 b=5:0', 'RGB 5:6:5 by masks')
    fb = fake_framebuffer(t, boot_fake_gop(t, 'resolution: 320x200x16'))
    t.expect_eq(fb, '320x200x16 pitch=640 model=1 r=5:10 g=5:5 b=5:0', '5:5:5 with a reserved bit: 16 bits')


def boot_fake_gop_only(t, flags):
    """The probe's two GOPs alone, the machine having no display of its own."""
    m = probe_then_limine(t, f'fakegop:2:{flags}',
                          'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x'),
                          {'/boot/kernel.elf': HostFile(kernel_file(t, 'full'))}, display=False)
    r = t.kernel_report(m)
    check_report_clean(t, r)
    m.stop()
    return r


@check(ports=UEFI_PORTS, tags=['limine-protocol', 'gop', 'hostile'])
def gop_not_started_or_refusing(t):
    """[intent] A GOP whose QueryMode() says EFI_NOT_STARTED, as some
    firmware's did until SetMode() was called, is started in its first mode
    where no framebuffer was found before it, and passed over otherwise; a
    mode SetMode() refuses is passed over for another."""
    r = boot_fake_gop_only(t, 1)
    t.expect_eq(r.int('framebuffer.count'), 1, 'one framebuffer: the first GOP started, the second passed over')
    t.expect_eq(r['framebuffer.0'], '800x600x32 pitch=3200 model=1 r=8:0 g=8:8 b=8:16', 'the EDID mode, once started')
    r = boot_fake_gop_only(t, 2)
    fb = r.get('framebuffer.0', '')
    t.check(r.int('framebuffer.count') == 1 and fb and not fb.startswith('800x600'),
            f'the EDID mode refused, another taken: {fb}')


@check(ports=UEFI_PORTS, tags=['limine-protocol', 'gop', 'hostile'])
def gop_framebuffer_in_boot_services_data(t):
    """[user] A framebuffer in boot services data, here half a page into its
    pages, which the memory map hands the OS once boot services exit, is
    passed on as framebuffer memory with the free memory around it cut back
    to whole pages, instead of stopping the boot."""
    r = boot_fake_gop_only(t, 4)
    t.expect_eq(r.int('framebuffer.count'), 1, 'one framebuffer: the second GOP, over the same memory, left out')
    t.expect_eq(int(r['framebuffer.address'], 16) & 0xfff, 0x800, 'the fake framebuffer, half a page in')
