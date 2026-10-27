import struct

from harness.api import check
from harness.media import HostFile
from harness.ports import X86_PORTS
from cases._common import boot_entry, entry, panics_in_turn, read
from cases.multiboot import (MB2_MODULES, boot_mb2, check_mb2_info, h, kernel_range, mb2_header_at, mb2_kernel,
                             mb2_report)

# More of Multiboot 2 [spec: the Multiboot2 Specification 2.0]: kernels in
# ELF64 files, the relocatable tag's search when its first place is taken,
# the 64-bit SMBIOS entry point, and headers and paths Limine refuses
# [intent: Limine's messages].


def header_tags(data):
    """{type: offset} of the tags of a kernel's Multiboot 2 header."""
    at = mb2_header_at(data)
    _, _, length, _ = struct.unpack_from('<IIII', data, at)
    tags, i = {}, at + 16
    while i < at + length:
        typ, _, size = struct.unpack_from('<HHI', data, i)
        if typ == 0:
            break
        tags[typ] = i
        i += (size + 7) & ~7
    return tags


def patched(data, at, fmt, *values):
    data = bytearray(data)
    struct.pack_into(fmt, data, at, *values)
    return bytes(data)


@check(ports=X86_PORTS, tags=['multiboot2', 'hostile'], timeout=600)
def mb2_more_errors(t):
    """A missing kernel or module, an information request tag too short to
    be one, and an address tag whose image runs past 4GiB: each refused,
    the menu coming back."""
    elf = read(mb2_kernel(t))
    aout = read(mb2_kernel(t, 'aout', 'kernel.bin'))
    info = header_tags(elf)[1]
    addr = header_tags(aout)[2]
    wrapped = patched(aout, addr + 8, '<IIII', 0xffffff00, 0xffffff00, 0, 0xffffffff)
    past = ('multiboot2: load_addr + load_size overflow' if t.port.name in ('bios', 'uefi-ia32')
            else 'multiboot2: Illegal bss end address')
    cases = [
        (entry('M0', protocol='multiboot2', path='boot():/missing'),
         'multiboot2: Failed to open executable with path `boot():/missing`. Is the path correct?'),
        (entry('M1', protocol='multiboot2', path='boot():/boot/elf', modules=[('boot():/missing.bin', None)]),
         'multiboot2: Failed to open module with path `boot():/missing.bin`. Is the path correct?'),
        (entry('M2', protocol='multiboot2', path='boot():/boot/short'),
         'multiboot2: Invalid information request tag size'),
        (entry('M3', protocol='multiboot2', path='boot():/boot/wrapped'), past),
    ]
    files = {'/boot/elf': elf, '/boot/short': patched(elf, info + 4, '<I', 4), '/boot/wrapped': wrapped}
    m, _ = boot_entry(t, 'timeout: no\n' + ''.join(text for text, _ in cases), files=files, kernel_variant=None)
    for (text, want), got in zip(cases, panics_in_turn(t, m, len(cases))):
        t.check(got == want, f'{text.split()[0]}: {got!r}, want {want!r}')


@check(ports=X86_PORTS, tags=['multiboot2', 'elf'])
def mb2_elf64_container(t):
    """[intent] The same kernels in ELF64 files: loaded alike, their section
    headers passed in the 64-bit form, and a relocated one's allocated
    sections given where they went."""
    path = mb2_kernel(t, 'elf', 'kernel64.elf')
    m = boot_mb2(t, HostFile(path), modules=MB2_MODULES)
    r = mb2_report(t, m)
    check_mb2_info(t, r, 'mb2 command line', MB2_MODULES, kernel_range(mb2_kernel(t)), elf=read(path))
    t.expect_eq(h(r, 'elf.entsize'), 64, 'ELF64 section headers')
    m.stop()
    path = mb2_kernel(t, 'relochigh', 'kernel64.elf')
    lo, hi = kernel_range(mb2_kernel(t, 'relochigh'))
    m = boot_mb2(t, HostFile(path))
    r = mb2_report(t, m)
    base = h(r, 'load_base')
    check_mb2_info(t, r, 'mb2 command line', [], (base, base + hi - lo), elf=read(path), load_base=base)


@check(ports=X86_PORTS, tags=['multiboot2'])
def mb2_relocation_search(t):
    """The relocatable tag's search moves on from places that are not free
    RAM: up from the legacy video memory at 640KiB when the lowest is
    preferred, down from the top of 4GiB when the highest is, by its
    alignment, until the image fits [spec]."""
    for variant, lo_addr, hi_addr, align in (('reloclow', 0xa0000, 0x1000000, 0x10000),
                                             ('relochigh', 0x400000, 0xffffffff, 0x200000)):
        data = read(mb2_kernel(t, variant))
        tag = header_tags(data)[10]
        image = patched(data, tag + 8, '<III', lo_addr, hi_addr, align)
        lo, hi = kernel_range(mb2_kernel(t, variant))
        m = boot_mb2(t, image)
        r = mb2_report(t, m)
        base = h(r, 'load_base')
        t.check(lo_addr <= base and base + hi - lo <= hi_addr, f'{variant}: {base:#x} within min and max')
        t.check(base >= 0x100000 if variant == 'reloclow' else base + hi - lo <= 0xe0000000,
                f'{variant}: {base:#x} past the hole it started in')
        t.expect_eq(base % align, 0, f'{variant}: aligned')
        check_mb2_info(t, r, 'mb2 command line', [], (base, base + hi - lo), elf=image, load_base=base)
        m.stop()


@check(ports=X86_PORTS, tags=['multiboot2', 'hostile'])
def mb2_relocation_from_zero(t):
    """A relocatable tag whose min_addr is 0, with the lowest place or none
    preferred, does not have the image put over physical page 0, where GRUB
    does not put it either [user]; otherwise the image goes where the tag
    lets it (Multiboot2 2.0 section 3.1.13) [spec]."""
    for variant in ('reloczero', 'reloczeronone'):
        path = mb2_kernel(t, variant)
        lo, hi = kernel_range(path)
        m = boot_mb2(t, HostFile(path))
        r = mb2_report(t, m)
        base = h(r, 'load_base')
        t.check(base >= 0x1000 and base + hi - lo <= 0x1000000, f'{variant}: {base:#x} past page 0, under max_addr')
        t.expect_eq(base % 0x1000, 0, f'{variant}: aligned')
        check_mb2_info(t, r, 'mb2 command line', [], (base, base + hi - lo), elf=read(path), load_base=base)
        m.stop()


@check(ports=X86_PORTS, tags=['multiboot2'])
def mb2_smbios3_and_resolution(t):
    """With only a 64-bit SMBIOS entry point the SMBIOS tag carries version
    3 and its structure table [spec]; resolution overrides what the
    framebuffer tag asks for (CONFIG.md); verbose lists the modules."""
    m = boot_mb2(t, HostFile(mb2_kernel(t, 'fb')), modules=MB2_MODULES,
                 extra='resolution: 640x480x32\n', machine_opts=['smbios-entry-point-type=64'])
    r = mb2_report(t, m)
    major, minor, first, length = (int(x, 16) for x in r.multi['smbios'][0].split())
    t.expect_eq(major, 3, f'SMBIOS 3 ({major}.{minor})')
    t.check((first >> 8) & 0xff >= 4, f'the structure table, not an entry point: starts {first:08x}')
    t.expect_eq((h(r, 'fb.width'), h(r, 'fb.height')), (640, 480), 'the configured resolution')


@check(ports=['uefi-ia32', 'uefi-x86-64'], tags=['multiboot2', 'hostile'])
def mb2_smbios3_table_wraps(t):
    """[intent] A 64-bit SMBIOS entry point whose structure table runs past
    the top of the address space gives no SMBIOS tag, rather than one copied
    from where the address wraps to; the 32-bit entry point still gives its
    own."""
    from cases.chainload import probe_then_limine
    config = 'timeout: 0\n' + entry('MB2', protocol='multiboot2', path='boot():/kernel', cmdline='x')
    m = probe_then_limine(t, 'smbios3:fffffffffffff000:2000', config, {'/kernel': HostFile(mb2_kernel(t))})
    r = mb2_report(t, m)
    t.check('K smbios3=0' in m.stream(t.kernel_channel).text().splitlines(), 'the probe put its entry point in place')
    majors = [int(v.split()[0], 16) for v in r.multi.get('smbios', [])]
    t.expect_eq(majors, [2], 'only the 32-bit entry point gives a tag')
