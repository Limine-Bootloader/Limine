import struct

from harness.api import check
from harness.media import HostFile, Tree
from harness.ports import X86_PORTS, Drive
from harness.report import Report, fnv1a, unescape
from cases._common import descriptor, entry, read, wait_panic

# Multiboot 1: expectations marked [spec] come from the Multiboot
# Specification 0.6.96, [intent] ones are choices Limine's code states.

MB1_MAGIC = 0x2badb002
KERNEL_BASE = 0x300000


def mb1_kernel(t, variant='elf', name='kernel.elf'):
    return t.env.payload('multiboot', 'mb1', variant, name)


def elf_sections(data):
    """(type, flags, addr, offset, size) of each section header of an ELF."""
    is64 = data[4] == 2
    if is64:
        shoff, = struct.unpack_from('<Q', data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3a)
    else:
        shoff, = struct.unpack_from('<I', data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2e)
    out = []
    for i in range(shnum):
        off = shoff + i * shentsize
        if is64:
            _, typ, flags, addr, offset, size = struct.unpack_from('<IIQQQQ', data, off)
        else:
            _, typ, flags, addr, offset, size = struct.unpack_from('<IIIIII', data, off)
        out.append((typ, flags, addr, offset, size))
    return {'shentsize': shentsize, 'shnum': shnum, 'shstrndx': shstrndx, 'sections': out}


def boot_mb1(t, kernel, cmdline='mb1 command line', modules=(), extra='', protocol='multiboot1', **machine):
    tree = Tree()
    tree.add('/boot/kernel', kernel)
    mods = []
    for path, data, string in modules:
        tree.add(path, data)
        mods.append((f'boot():{path}', string))
    config = 'timeout: 0\n' + entry('MB1', protocol=protocol, path='boot():/boot/kernel', cmdline=cmdline,
                                    modules=mods, extra=extra)
    t.limine_files(config, tree)
    return t.boot(t.boot_disk(tree), **machine)


def mb1_report(t, m, begin='B MB1-TEST'):
    m.wait_for('debugcon', 'E DONE', 60)
    m.wait_for('debugcon', '\n', 5, start=m.stream('debugcon').text().rfind('E DONE'))
    return Report(m.stream('debugcon').text(), begin)


def h(r, key):
    return int(r[key], 16)


def check_machine_state(t, r):
    """Multiboot 0.6.96 section 3.2: what the CPU is in when the kernel is
    entered."""
    t.expect_eq(h(r, 'eax'), MB1_MAGIC, '[spec] EAX is the magic value')
    cs = descriptor(h(r, 'gdt.cs'))
    t.expect_eq((cs['present'], cs['s'], cs['type'] & 0b1010, cs['db'], cs['base'], cs['limit']),
                (1, 1, 0b1010, 1, 0, 0xffffffff), '[spec] CS a 32-bit read/execute segment, offset 0, limit 4GiB')
    for seg in ('ds', 'es', 'fs', 'gs', 'ss'):
        d = descriptor(h(r, f'gdt.{seg}'))
        t.expect_eq((d['present'], d['s'], d['type'] & 0b1110, d['db'], d['base'], d['limit']),
                    (1, 1, 0b0010, 1, 0, 0xffffffff),
                    f'[spec] {seg.upper()} a 32-bit read/write segment, offset 0, limit 4GiB')
    t.expect_eq(h(r, 'a20'), 1, '[spec] A20 enabled')
    t.expect_eq(h(r, 'cr0') & 0x80000001, 1, '[spec] CR0: PG clear, PE set')
    t.expect_eq(h(r, 'eflags') & ((1 << 17) | (1 << 9)), 0, '[spec] EFLAGS: VM and IF clear')


def mmap_entries(r):
    return [tuple(int(x, 16) for x in v.split()) for v in r.multi.get('mmap', [])]


def check_mbi(t, r, cmdline, modules, kernel_range, elf=None, page_aligned_spec=False):
    """Multiboot 0.6.96 section 3.3: the information structure."""
    t.check(h(r, 'ebx') != 0, '[spec] EBX holds the information structure')
    flags = h(r, 'mbi.flags')
    t.expect_eq(flags & ~0x1fff, 0, '[spec] undefined flags bits zero')
    t.check(flags & 0x30 != 0x30, '[spec] flags bits 4 and 5 are mutually exclusive')
    t.check(flags & 1, '[spec] mem_* given (header flags bit 1)')
    t.check(h(r, 'mbi.mem_lower') <= 640, f'[spec] mem_lower at most 640KiB: {h(r, "mbi.mem_lower")}')

    t.check(flags & (1 << 6), '[intent] memory map given')
    entries = mmap_entries(r)
    t.expect_eq(h(r, 'mmap.length'), 24 * len(entries), '[spec] mmap_length covers the size/entry pairs')
    t.check(all(e[0] == 20 for e in entries), f'[spec] each entry 20 bytes: {entries}')
    t.check(all(e[3] in (1, 2, 3, 4, 5) for e in entries), f'[intent] standard E820 types only: {entries}')
    usable = sum(e[2] for e in entries if e[3] == 1)
    t.check(usable >= 400 << 20, f'[spec] all standard RAM of the 512MiB listed: {usable:#x} available')
    # The highest address reached from 1MiB through available RAM bounds
    # mem_upper.
    top = 0x100000
    for base, length in sorted((e[1], e[2]) for e in entries if e[3] == 1):
        if base <= top < base + length:
            top = base + length
    t.check(h(r, 'mbi.mem_upper') * 1024 + 0x100000 <= top,
            f'[spec] mem_upper ({h(r, "mbi.mem_upper")}KiB) not past the first hole above 1MiB ({top:#x})')
    lo, hi = kernel_range
    t.check(any(e[3] == 1 and e[1] <= lo and hi <= e[1] + e[2] for e in entries),
            f'[intent] the kernel ({lo:#x}-{hi:#x}) is in available RAM of the raw map')

    t.check(flags & (1 << 2), '[intent] command line given')
    t.expect_eq(unescape(r['mbi.cmdline']), cmdline, '[spec] command line')

    if t.port.is_uefi:
        t.check(not flags & (1 << 1), '[spec] no boot_device: not loaded from a BIOS disk')
    else:
        t.check(flags & (1 << 1), '[intent] boot_device given, as GRUB gives it')
        t.expect_eq(h(r, 'mbi.boot_device'), 0x8000ffff,
                    '[spec] boot_device: drive 0x80, 0-based partition 0, the rest 0xff (GRUB\'s encoding)')
    t.check(flags & (1 << 9), '[intent] boot loader name given')
    t.check(unescape(r['mbi.boot_loader_name']).startswith('Limine '),
            f'[intent] boot loader name: {r.get("mbi.boot_loader_name")}')

    claimed = [(lo, hi, 'kernel')]
    mbi = h(r, 'ebx')
    claimed.append((mbi, mbi + 120, 'information structure'))
    if modules:
        t.check(flags & (1 << 3), '[spec] modules given')
        t.expect_eq(h(r, 'mods.count'), len(modules), '[spec] mods_count')
        mods = [tuple(int(x, 16) for x in v.split()) for v in r.multi['mod']]
        strings = [unescape(s) for s in r.multi.get('mod.string', [])]
        for i, ((path, data, string), (start, end, reserved, digest)) in enumerate(zip(modules, mods)):
            t.expect_eq(end - start, len(data), f'[intent] module {i} spans its file, mod_end exclusive')
            t.expect_eq(f'{digest:016x}', fnv1a(data), f'[spec] module {i} contents')
            t.expect_eq(reserved, 0, f'[spec] module {i} reserved field zero')
            t.expect_eq(strings[i], string or '', f'[spec] module {i} string')
            label = '[spec]' if page_aligned_spec else '[intent]'
            t.expect_eq(start % 4096, 0, f'{label} module {i} page aligned')
            claimed.append((start, end, f'module {i}'))
    elif flags & (1 << 3):
        t.expect_eq(h(r, 'mods.count'), 0, '[spec] no modules')

    if elf is not None:
        t.check(flags & (1 << 5), '[spec] ELF section headers given for an ELF kernel')
        info = elf_sections(elf)
        t.expect_eq((h(r, 'elf.num'), h(r, 'elf.size'), h(r, 'elf.shndx')),
                    (info['shnum'], info['shentsize'], info['shstrndx']), '[spec] num, size, shndx as in the ELF')
        got = {int(v.split()[0], 16): tuple(int(x, 16) for x in v.split()[1:]) for v in r.multi['elf.section']}
        for i, (typ, sflags, addr, offset, size) in enumerate(info['sections']):
            if i == 0 or size == 0:
                continue
            gtype, gflags, gaddr, gsize, digest = got[i]
            t.expect_eq((gtype, gflags, gsize), (typ, sflags, size), f'[spec] section {i} header')
            if sflags & 2:
                t.expect_eq(gaddr, addr, f'[spec] allocated section {i} where the ELF places it')
                # Writable ones the kernel has written to by now.
                if typ != 8 and not sflags & 1:
                    t.expect_eq(f'{digest:016x}', fnv1a(elf[offset:offset + size]), f'[spec] section {i} contents')
            else:
                t.check(gaddr != 0, f'[spec] section {i} loaded: all sections are')
                t.expect_eq(f'{digest:016x}', fnv1a(elf[offset:offset + size]), f'[spec] section {i} contents')
                claimed.append((gaddr, gaddr + gsize, f'section {i}'))
    else:
        t.check(not flags & (1 << 5), '[spec] no ELF section headers for a kernel that is not ELF')

    claimed.sort()
    for (a0, a1, an), (b0, b1, bn) in zip(claimed, claimed[1:]):
        t.check(a1 <= b0, f'[spec] {an} ({a0:#x}-{a1:#x}) and {bn} ({b0:#x}-{b1:#x}) do not overlap')


def kernel_range(path):
    """Where the check kernel is linked: 3MiB up to the end of its bss."""
    data = open(path, 'rb').read()
    info = elf_sections(data)
    end = max(addr + size for typ, flags, addr, offset, size in info['sections'] if flags & 2)
    return KERNEL_BASE, end


MODULES = [('/boot/mod1.bin', b'first multiboot module\n' * 50, 'first module string'),
           ('/boot/mod2.bin', bytes(range(256)) * 9 + b'tail', None)]


@check(ports=X86_PORTS, tags=['multiboot1'])
def mb1_elf(t):
    """An ELF kernel with modules: machine state and every information
    structure field Multiboot 0.6.96 defines."""
    path = mb1_kernel(t)
    m = boot_mb1(t, HostFile(path), modules=MODULES)
    r = mb1_report(t, m)
    check_machine_state(t, r)
    data = open(path, 'rb').read()
    check_mbi(t, r, 'mb1 command line', MODULES, kernel_range(path), elf=data)
    t.expect_eq(h(r, 'bss.zero'), 1, '[spec] bss zeroed (ELF: memory past the file image)')


@check(ports=X86_PORTS, tags=['multiboot1'])
def mb1_elf64_container(t):
    """[intent] The same 32-bit kernel in an ELF64 file loads too; its
    section headers are passed in their 64-bit form."""
    path = mb1_kernel(t, 'elf', 'kernel64.elf')
    m = boot_mb1(t, HostFile(path), cmdline='', modules=[])
    r = mb1_report(t, m)
    check_machine_state(t, r)
    data = open(path, 'rb').read()
    check_mbi(t, r, '', [], kernel_range(mb1_kernel(t)), elf=data)
    t.expect_eq(h(r, 'elf.size'), 64, 'ELF64 section headers')


@check(ports=X86_PORTS, tags=['multiboot1'])
def mb1_page_aligned_modules(t):
    """Header flags bit 0: modules page aligned."""
    path = mb1_kernel(t, 'align')
    m = boot_mb1(t, HostFile(path), modules=MODULES)
    r = mb1_report(t, m)
    check_mbi(t, r, 'mb1 command line', MODULES, kernel_range(path), elf=open(path, 'rb').read(),
              page_aligned_spec=True)


@check(ports=X86_PORTS, tags=['multiboot1'])
def mb1_aout_kludge(t):
    """Header flags bit 16: a flat image placed and entered by the address
    fields, its bss zeroed, with load_end_addr given or 0 (whole file)."""
    for variant in ('aout', 'aoutwhole'):
        elf = mb1_kernel(t, variant)
        m = boot_mb1(t, HostFile(mb1_kernel(t, variant, 'kernel.bin')), modules=MODULES[:1])
        r = mb1_report(t, m)
        check_machine_state(t, r)
        check_mbi(t, r, 'mb1 command line', MODULES[:1], kernel_range(elf))
        t.expect_eq(h(r, 'bss.zero'), 1, f'[spec] {variant}: bss zeroed by the loader')
        m.stop()


@check(ports=X86_PORTS, tags=['multiboot1'])
def mb1_framebuffer(t):
    """Header flags bit 2 with a preferred 800x600x32 linear mode."""
    m = boot_mb1(t, HostFile(mb1_kernel(t, 'video')))
    r = mb1_report(t, m)
    t.check(h(r, 'mbi.flags') & (1 << 12), '[spec] framebuffer information given (header flags bit 2)')
    t.expect_eq(h(r, 'fb.type'), 1, '[spec] direct RGB colour')
    t.expect_eq((h(r, 'fb.width'), h(r, 'fb.height'), h(r, 'fb.bpp')), (800, 600, 32),
                '[intent] the preferred mode, which is available')
    t.check(h(r, 'fb.addr') < 1 << 32, '[spec] framebuffer below 4GiB where possible')
    t.check(h(r, 'fb.pitch') >= 800 * 4, f'[spec] pitch {h(r, "fb.pitch")} holds a line')
    colour = bytes.fromhex(r['fb.colour'])
    t.expect_eq(sorted([(colour[0], colour[1]), (colour[2], colour[3]), (colour[4], colour[5])]),
                [(0, 8), (8, 8), (16, 8)], '[spec] RGB field positions and sizes, at offset 112')
    t.expect_eq(h(r, 'fb.drawn'), 1, 'kernel drew its pattern')
    img = m.screendump('pattern')
    for x, rgb, name in ((32, 0xff0000, 'red'), (96, 0x00ff00, 'green'), (160, 0x0000ff, 'blue'),
                         (224, 0xffffff, 'white')):
        t.expect_eq(img.pixel(x, 32), rgb, f'[spec] the framebuffer described is the one shown: {name} square')


@check(ports=X86_PORTS, tags=['multiboot1'])
def mb1_text_mode(t):
    """Header flags bit 2 asking for EGA text: given on BIOS; [intent] UEFI
    has no text mode and sets a graphics one."""
    m = boot_mb1(t, HostFile(mb1_kernel(t, 'text')))
    r = mb1_report(t, m)
    t.check(h(r, 'mbi.flags') & (1 << 12), '[spec] framebuffer information given (header flags bit 2)')
    if t.port.is_uefi:
        t.expect_eq(h(r, 'fb.type'), 1, '[intent] a linear framebuffer instead')
        return
    t.expect_eq((h(r, 'fb.type'), h(r, 'fb.addr'), h(r, 'fb.width'), h(r, 'fb.height'), h(r, 'fb.bpp'),
                 h(r, 'fb.pitch')), (2, 0xb8000, 80, 25, 16, 160), '[spec] EGA text, in characters')
    scr = t.screen(m, 'text')
    t.check(scr.row_text(0).startswith('MB1 TEXT OK'), f'[spec] text memory shown: {scr.row_text(0)!r}')


@check(ports=X86_PORTS, tags=['multiboot1'])
def mb1_no_video_request(t):
    """[intent] Without header flags bit 2, as GRUB does: BIOS is left in VGA
    text mode, described as EGA text; UEFI sets no mode and describes none."""
    m = boot_mb1(t, HostFile(mb1_kernel(t)))
    r = mb1_report(t, m)
    if t.port.is_uefi:
        t.check(not h(r, 'mbi.flags') & (1 << 12), 'no framebuffer information')
        return
    t.check(h(r, 'mbi.flags') & (1 << 12), 'framebuffer information given')
    t.expect_eq((h(r, 'fb.type'), h(r, 'fb.addr'), h(r, 'fb.width'), h(r, 'fb.height'), h(r, 'fb.bpp'),
                 h(r, 'fb.pitch')), (2, 0xb8000, 80, 25, 16, 160), '[spec] EGA text, in characters')
    img = m.screendump('text')
    t.expect_eq((img.width, img.height), (720, 400), 'VGA 80x25 text mode')


@check(ports=['bios'], tags=['multiboot1'])
def mb1_pic(t):
    """[spec] The PICs are left as BIOS programs them: IRQ0 at vector 8,
    IRQ8 at vector 0x70."""
    m = boot_mb1(t, HostFile(mb1_kernel(t, 'pic')))
    r = mb1_report(t, m)
    t.expect_eq(h(r, 'pic.irq0_vector'), 0x08, 'IRQ0 vector')
    t.expect_eq(h(r, 'pic.irq8_vector'), 0x70, 'IRQ8 vector')


@check(ports=['bios'], tags=['multiboot1', 'hostile'])
def mb1_a20_turned_off(t):
    """[spec] A20 is enabled for the kernel even when Limine is started with
    the gate off, a boot record before it having turned it off: Limine turns
    it on through the BIOS, and through the keyboard controller when the
    BIOS refuses [intent]. (Its last resort, port 0x92 with no keyboard
    controller, QEMU cannot give: its port 0x92 goes with its 8042.)"""
    tree = Tree()
    tree.add('/boot/kernel', HostFile(mb1_kernel(t)))
    t.limine_files('timeout: 0\n' + entry('MB1', protocol='multiboot1', path='boot():/boot/kernel', cmdline='x'),
                   tree)
    limine = t.boot_disk(tree)
    for what, record in (('the BIOS', 'a20off.bin'), ('the keyboard controller', 'a20off-noint15.bin')):
        path = t.path(f'{record}.img')
        with open(path, 'wb') as f:
            f.write(read(t.env.payload('bios', record)).ljust(1 << 20, b'\0'))
        m = t.boot(drives=[Drive(path, bootindex=1), Drive(limine.path)])
        r = mb1_report(t, m)
        t.check('A20 off before Limine' in m.stream('debugcon').text(), f'{what}: the gate off as Limine started')
        t.expect_eq(h(r, 'a20'), 1, f'{what}: A20 enabled')
        m.stop()


def flat_image(t, offset):
    return open(t.env.payload('multiboot', 'mb1', f'hdr{offset}.bin'), 'rb').read()


def patched(data, at, *words):
    data = bytearray(data)
    for i, w in enumerate(words):
        if w is not None:
            struct.pack_into('<I', data, at + 4 * i, w & 0xffffffff)
    return bytes(data)


def with_flags(data, at, flags):
    """A header at at with new flags and the checksum to match."""
    return patched(data, at + 4, flags, -(0x1badb002 + flags))


def elf_with_header_flags(t, flags):
    data = open(mb1_kernel(t), 'rb').read()
    return with_flags(data, data.find(struct.pack('<I', 0x1badb002)), flags)


@check(ports=X86_PORTS, tags=['multiboot1'])
def mb1_header_placement(t):
    """[spec] The header lies wholly within the first 8192 bytes."""
    m = boot_mb1(t, flat_image(t, 8160))
    mb1_report(t, m, 'B MB1-FLAT')
    m.stop()
    for offset, message in ((8164, 'multiboot1: Header extends past the first 8192 bytes'),
                            (8180, 'multiboot1: Header extends past the first 8192 bytes'),
                            (8184, 'multiboot1: Invalid magic'),
                            (8192, 'multiboot1: Invalid magic')):
        m = boot_mb1(t, flat_image(t, offset))
        msg, _ = wait_panic(t, m)
        t.expect_eq(msg, message, f'header at {offset}')
        m.stop()


@check(ports=X86_PORTS, tags=['multiboot1'])
def mb1_optional_flags_ignored(t):
    """[spec] Bits 16-31 a loader does not know it may ignore."""
    m = boot_mb1(t, elf_with_header_flags(t, 0x2 | (1 << 17)))
    r = mb1_report(t, m)
    t.expect_eq(h(r, 'eax'), MB1_MAGIC, 'booted')


@check(ports=X86_PORTS, tags=['multiboot1'])
def mb1_errors(t):
    hdr = 8160
    base = flat_image(t, hdr)
    fields = hdr + 12
    elf = open(mb1_kernel(t), 'rb').read()
    cases = [
        (None, 'multiboot1: Executable path not specified'),
        ('/missing', 'multiboot1: Failed to open executable with path `boot():/missing`. Is the path correct?'),
        (b'\x02\xb0\xad\x1b', 'multiboot1: Kernel file too small to contain header'),
        (b'\x00' * 4096, 'multiboot1: Invalid magic'),
        (patched(base, hdr + 8, 0), 'multiboot1: Header checksum is invalid'),
        (with_flags(base, hdr, 0x10000 | (1 << 3)), 'multiboot1: Header requires unsupported features'),
        (with_flags(base, hdr, 0x10000 | (1 << 15)), 'multiboot1: Header requires unsupported features'),
        (patched(base, fields, None, KERNEL_BASE + hdr + 4), 'multiboot1: Illegal load address'),
        (patched(base, fields, KERNEL_BASE + hdr, KERNEL_BASE - 4), 'multiboot1: Address tag offset underflow'),
        (patched(base, fields, None, None, KERNEL_BASE - 1), 'multiboot1: Load end address less than load address'),
        (patched(base, fields, None, None, KERNEL_BASE + 0x100, KERNEL_BASE + 0x80),
         'multiboot1: Illegal bss end address'),
        (patched(base, fields, None, None, KERNEL_BASE + len(base) + 4),
         'multiboot1: load_src + load_size exceeds kernel file size'),
        (patched(base, fields, None, None, None, None, KERNEL_BASE + len(base) + 0x1000),
         'multiboot1: Entry point is outside the executable'),
        (patched(base, fields, 0xf0000000 + hdr, 0xf0000000, 0, 0, 0xf0000000),
         'multiboot1: Executable load address is not usable memory'),
        (patched(base, fields, 0xffffe000 + hdr, 0xffffe000, 0, 0, 0xffffe000),
         'multiboot1: Executable does not fit under 4GiB'),
        (elf[:4] + b'\x03' + elf[5:], 'multiboot1: Invalid ELF file bitness'),
    ]
    for data, message in cases:
        tree = Tree()
        if isinstance(data, bytes):
            tree.add('/boot/kernel', data)
            text = entry('MB1', protocol='multiboot1', path='boot():/boot/kernel')
        elif data is None:
            text = entry('MB1', protocol='multiboot1')
        else:
            text = entry('MB1', protocol='multiboot1', path=f'boot():{data}')
        t.limine_files('timeout: 0\n' + text, tree)
        m = t.boot(t.boot_disk(tree))
        msg, _ = wait_panic(t, m)
        t.expect_eq(msg, message, message)
        m.stop()
    tree = Tree({'/boot/kernel': HostFile(mb1_kernel(t))})
    t.limine_files('timeout: 0\n' + entry('MB1', protocol='multiboot1', path='boot():/boot/kernel',
                                          modules=[('boot():/missing.bin', None)]), tree)
    m = t.boot(t.boot_disk(tree))
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'multiboot1: Failed to open module with path `boot():/missing.bin`. Is the path correct?',
                'missing module')


# Multiboot 2: [spec] is the Multiboot2 Specification 2.0.

MB2_MAGIC = 0x36d76289


def mb2_kernel(t, variant='elf', name='kernel.elf'):
    return t.env.payload('multiboot', 'mb2', variant, name)


def boot_mb2(t, kernel, cmdline='mb2 command line', modules=(), extra='', **machine):
    return boot_mb1(t, kernel, cmdline, modules, extra, protocol='multiboot2', **machine)


def mb2_report(t, m, begin='B MB2-TEST'):
    return mb1_report(t, m, begin)


def mb2_tags(r):
    """(offset, type, size) of each tag, in order."""
    return [tuple(int(x, 16) for x in v.split()) for v in r.multi.get('tag', [])]


def check_mb2_machine_state(t, r):
    """Multiboot2 2.0 section 3.3: the i386 machine state, which on EFI also
    means boot services terminated."""
    t.expect_eq(h(r, 'eax'), MB2_MAGIC, '[spec] EAX is the magic value')
    cs = descriptor(h(r, 'gdt.cs'))
    t.expect_eq((cs['present'], cs['s'], cs['type'] & 0b1010, cs['db'], cs['base'], cs['limit']),
                (1, 1, 0b1010, 1, 0, 0xffffffff), '[spec] CS a 32-bit read/execute segment, offset 0, limit 4GiB')
    for seg in ('ds', 'es', 'fs', 'gs', 'ss'):
        d = descriptor(h(r, f'gdt.{seg}'))
        t.expect_eq((d['present'], d['s'], d['type'] & 0b1110, d['db'], d['base'], d['limit']),
                    (1, 1, 0b0010, 1, 0, 0xffffffff),
                    f'[spec] {seg.upper()} a 32-bit read/write segment, offset 0, limit 4GiB')
    t.expect_eq(h(r, 'a20'), 1, '[spec] A20 enabled')
    t.expect_eq(h(r, 'cr0') & 0x80000001, 1, '[spec] CR0: PG clear, PE set')
    t.expect_eq(h(r, 'eflags') & ((1 << 17) | (1 << 9)), 0, '[spec] EFLAGS: VM and IF clear')
    t.expect_eq(h(r, 'efi_entry'), 0, '[intent] entered at the i386 entry, boot services terminated')


def check_mb2_info(t, r, cmdline, modules, kernel_range, elf=None, page_aligned_spec=False, load_base=None):
    """Multiboot2 2.0 section 3.6: the information structure and its tags."""
    mbi = h(r, 'ebx')
    t.expect_eq(mbi % 8, 0, '[spec] information structure 8-byte aligned')
    t.expect_eq(h(r, 'mbi.reserved'), 0, '[spec] reserved field zero')
    tags = mb2_tags(r)
    t.check(all(off % 8 == 0 for off, _, _ in tags), f'[spec] tags 8-byte aligned: {tags}')
    t.check(tags and tags[-1][1:] == (0, 8), f'[spec] terminated by a type 0, size 8 tag: {tags[-1:]}')
    if tags:
        t.expect_eq(h(r, 'mbi.total_size'), tags[-1][0] + 8, '[spec] total_size runs to the end of the last tag')
    types = [typ for _, typ, _ in tags]

    t.check(1 in types, '[spec] command line tag, requested')
    t.expect_eq(unescape(r['cmdline']), cmdline, '[spec] command line')
    t.check(2 in types, '[spec] boot loader name tag, requested')
    t.check(unescape(r['boot_loader_name']).startswith('Limine '), f'[intent] boot loader name: {r.get("boot_loader_name")}')

    t.check(4 in types, '[spec] basic memory information, requested')
    t.check(h(r, 'mem_lower') <= 640, '[spec] mem_lower at most 640KiB')
    t.check(6 in types, '[spec] memory map, requested')
    t.expect_eq(h(r, 'mmap.entry_size'), 24, '[spec] entry_size 24, a multiple of 8')
    t.expect_eq(h(r, 'mmap.entry_version'), 0, '[spec] entry_version 0')
    entries = [tuple(int(x, 16) for x in v.split()) for v in r.multi.get('mmap', [])]
    t.check(all(e[3] == 0 for e in entries), '[spec] entries\' reserved fields zero')
    t.check(all(e[2] in (1, 2, 3, 4, 5) for e in entries), f'[intent] standard E820 types only: {entries}')
    usable = sum(e[1] for e in entries if e[2] == 1)
    t.check(usable >= 400 << 20, f'[spec] all standard RAM of the 512MiB listed: {usable:#x} available')
    top = 0x100000
    for base, length in sorted((e[0], e[1]) for e in entries if e[2] == 1):
        if base <= top < base + length:
            top = base + length
    t.check(h(r, 'mem_upper') * 1024 + 0x100000 <= top,
            f'[spec] mem_upper ({h(r, "mem_upper")}KiB) not past the first hole above 1MiB ({top:#x})')

    def in_ram(lo, hi):
        return any(e[2] == 1 and e[0] <= lo and hi <= e[0] + e[1] for e in entries)

    lo, hi = kernel_range
    claimed = [(lo, hi, 'kernel'), (mbi, mbi + h(r, 'mbi.total_size'), 'information structure')]
    if modules:
        mods = [tuple(int(x, 16) for x in v.split()) for v in r.multi.get('mod', [])]
        strings = [unescape(s) for s in r.multi.get('mod.string', [])]
        t.expect_eq(len(mods), len(modules), '[spec] one module tag per module')
        for i, ((path, data, string), (start, end, digest)) in enumerate(zip(modules, mods)):
            t.expect_eq(end - start, len(data), f'[intent] module {i} spans its file, mod_end exclusive')
            t.expect_eq(f'{digest:016x}', fnv1a(data), f'[spec] module {i} contents')
            t.expect_eq(strings[i], string or '', f'[spec] module {i} string')
            label = '[spec]' if page_aligned_spec else '[intent]'
            t.expect_eq(start % 4096, 0, f'{label} module {i} page aligned')
            claimed.append((start, end, f'module {i}'))
    else:
        t.check(3 not in types, '[spec] no module tags without modules')
    for a0, a1, what in claimed:
        t.check(a1 <= 1 << 32, f'[spec] {what} below 4GiB')
        t.check(in_ram(a0, a1), f'[spec] {what} ({a0:#x}-{a1:#x}) within available RAM of the map')

    if elf is not None:
        t.check(9 in types, '[spec] ELF sections tag for an ELF kernel')
        info = elf_sections(elf)
        t.expect_eq((h(r, 'elf.num'), h(r, 'elf.entsize'), h(r, 'elf.shndx')),
                    (info['shnum'], info['shentsize'], info['shstrndx']),
                    '[spec] num, entsize, shndx as in the ELF, u32 fields as in Multiboot2 2.0\'s multiboot2.h')
        got = {int(v.split()[0], 16): tuple(int(x, 16) for x in v.split()[1:]) for v in r.multi['elf.section']}
        slide = (load_base or KERNEL_BASE) - KERNEL_BASE
        for i, (typ, sflags, addr, offset, size) in enumerate(info['sections']):
            if i == 0 or size == 0:
                continue
            gtype, gflags, gaddr, gsize, digest = got[i]
            t.expect_eq((gtype, gflags, gsize), (typ, sflags, size), f'[spec] section {i} header')
            if sflags & 2:
                t.expect_eq(gaddr, addr + slide, f'[spec] allocated section {i} where it is in memory')
                if typ != 8 and not sflags & 1:
                    t.expect_eq(f'{digest:016x}', fnv1a(elf[offset:offset + size]), f'[spec] section {i} contents')
            else:
                t.check(gaddr != 0, f'[spec] section {i} loaded: all sections are')
                t.expect_eq(f'{digest:016x}', fnv1a(elf[offset:offset + size]), f'[spec] section {i} contents')
                claimed.append((gaddr, gaddr + gsize, f'section {i}'))
    else:
        t.check(9 not in types, '[intent] no ELF sections tag for a kernel loaded by its address tag')

    claimed.sort()
    for (a0, a1, an), (b0, b1, bn) in zip(claimed, claimed[1:]):
        t.check(a1 <= b0, f'[spec] {an} ({a0:#x}-{a1:#x}) and {bn} ({b0:#x}-{b1:#x}) do not overlap')

    # Firmware tables, copied or pointed at.
    if 15 in types:
        sig, csum, length, rev = (int(x, 16) for x in r['acpi_new'].split())
        t.expect_eq((sig, csum, length), (int.from_bytes(b'RSD PTR ', 'little'), 0, 36),
                    '[spec] new RSDP tag: an intact ACPI 2.0+ RSDP')
        t.check(rev >= 2, '[spec] new RSDP tag: revision 2 or later')
    if 14 in types:
        sig, csum, length, _ = (int(x, 16) for x in r['acpi_old'].split())
        t.expect_eq((sig, csum, length), (int.from_bytes(b'RSD PTR ', 'little'), 0, 20),
                    '[spec] old RSDP tag: an intact ACPI 1.0 RSDP')
    t.check(14 in types or 15 in types, '[spec] an RSDP tag, requested and available')
    t.check(13 in types, '[spec] SMBIOS tag, requested and available')
    if t.port.is_uefi:
        want = 12 if t.port.name == 'uefi-x86-64' else 11
        t.check(want in types, f'[spec] EFI system table pointer tag {want}')
        key = 'efi64.signature' if want == 12 else 'efi32.signature'
        t.expect_eq(h(r, key), 0x5453595320494249, '[spec] it points at the EFI system table')
        t.check(17 in types, '[spec] EFI memory map tag')
        t.expect_eq(h(r, 'efi_mmap.descr_vers'), 1, '[spec] EFI memory map descriptor version')
    else:
        t.check(not set(types) & {11, 12, 17, 19, 20}, '[spec] no EFI tags on BIOS')
    t.check(18 not in types, '[intent] boot services always terminated')
    t.check(not set(types) & {19, 20}, '[intent] no EFI image handle tags with boot services gone, as GRUB')
    t.check(21 in types, '[intent] load base tag, given to every executable as GRUB does')
    t.expect_eq(h(r, 'load_base_addr'), load_base or KERNEL_BASE, '[spec] load base: where the image is')
    if t.port.is_uefi:
        t.check(5 not in types, '[spec] no BIOS boot device tag: not loaded from a BIOS disk')
    else:
        t.check(5 in types, '[intent] BIOS boot device tag, as GRUB gives it')
        t.expect_eq(r['bootdev'], '0x80 0x0 0xffffffff', '[spec] drive 0x80, 0-based partition 0 (GRUB\'s encoding)')
    major, minor, first, length = (int(x, 16) for x in r.multi['smbios'][0].split())
    t.check(major >= 2, f'[spec] SMBIOS version {major}.{minor}')
    t.check(first & 0xff not in (0x5f,) and (first >> 8) & 0xff >= 4,
            f'[user] SMBIOS tag holds the structure table, not an entry point: starts {first:08x}')


MB2_MODULES = [('/boot/m1.bin', b'multiboot2 module one\n' * 70, 'module one'),
               ('/boot/m2.bin', bytes(range(255, -1, -1)) * 5, None)]


@check(ports=X86_PORTS, tags=['multiboot2'])
def mb2_elf(t):
    """An ELF kernel asking for every information tag Limine supports."""
    path = mb2_kernel(t)
    m = boot_mb2(t, HostFile(path), modules=MB2_MODULES)
    r = mb2_report(t, m)
    check_mb2_machine_state(t, r)
    check_mb2_info(t, r, 'mb2 command line', MB2_MODULES, kernel_range(path), elf=open(path, 'rb').read())
    t.expect_eq(h(r, 'bss.zero'), 1, '[spec] bss zeroed')


@check(ports=X86_PORTS, tags=['multiboot2'])
def mb2_module_alignment(t):
    """The module alignment tag: modules page aligned."""
    path = mb2_kernel(t, 'modalign')
    m = boot_mb2(t, HostFile(path), modules=MB2_MODULES)
    r = mb2_report(t, m)
    check_mb2_info(t, r, 'mb2 command line', MB2_MODULES, kernel_range(path), elf=open(path, 'rb').read(),
                   page_aligned_spec=True)


@check(ports=X86_PORTS, tags=['multiboot2'])
def mb2_address_tag(t):
    """The address and entry address tags place a flat image, its bss
    zeroed; a load_addr of -1 loads from the file's start."""
    for variant in ('aout', 'aoutm1'):
        m = boot_mb2(t, HostFile(mb2_kernel(t, variant, 'kernel.bin')), modules=MB2_MODULES[:1])
        r = mb2_report(t, m)
        check_mb2_machine_state(t, r)
        check_mb2_info(t, r, 'mb2 command line', MB2_MODULES[:1], kernel_range(mb2_kernel(t, variant)))
        t.expect_eq(h(r, 'load_base'), KERNEL_BASE, f'[spec] {variant}: loaded where the tag says')
        t.expect_eq(h(r, 'bss.zero'), 1, f'[spec] {variant}: bss zeroed by the loader')
        m.stop()


@check(ports=X86_PORTS, tags=['multiboot2'])
def mb2_relocatable(t):
    """The relocatable header tag: placed between min_addr and max_addr,
    aligned, lowest or highest first, and reported by the load base tag."""
    for variant, pref in (('reloclow', 1), ('relochigh', 2)):
        path = mb2_kernel(t, variant)
        lo, hi = kernel_range(path)
        size = hi - lo
        m = boot_mb2(t, HostFile(path))
        r = mb2_report(t, m)
        base = h(r, 'load_base')
        t.check(21 in [typ for _, typ, _ in mb2_tags(r)], f'[spec] {variant}: load base tag')
        t.expect_eq(h(r, 'load_base_addr'), base, f'[spec] {variant}: load base tag gives where it is')
        t.check(base >= 0x400000 and base + size <= 0x1000000, f'[spec] {variant}: {base:#x} within min and max')
        t.expect_eq(base % 0x200000, 0, f'[spec] {variant}: aligned')
        if not t.port.is_uefi:
            want = 0x400000 if pref == 1 else (0x1000000 - size) & ~0x1fffff
            t.expect_eq(base, want, f'[spec] {variant}: the {"lowest" if pref == 1 else "highest"} place, free on BIOS')
        check_mb2_info(t, r, 'mb2 command line', [], (base, base + size), elf=open(path, 'rb').read(), load_base=base)
        m.stop()


@check(ports=X86_PORTS, tags=['multiboot2'])
def mb2_framebuffer(t):
    """The framebuffer tag asking for 800x600x32."""
    m = boot_mb2(t, HostFile(mb2_kernel(t, 'fb')))
    r = mb2_report(t, m)
    t.check(8 in [typ for _, typ, _ in mb2_tags(r)], '[spec] framebuffer info tag')
    t.expect_eq(h(r, 'fb.type'), 1, '[spec] direct RGB colour')
    t.expect_eq((h(r, 'fb.width'), h(r, 'fb.height'), h(r, 'fb.bpp')), (800, 600, 32),
                '[intent] the preferred mode, which is available')
    t.expect_eq(h(r, 'fb.reserved'), 0, '[spec] reserved zero')
    t.check(h(r, 'fb.addr') < 1 << 32, '[spec] framebuffer below 4GiB where possible')
    colour = bytes.fromhex(r['fb.colour'])
    t.expect_eq(sorted([(colour[0], colour[1]), (colour[2], colour[3]), (colour[4], colour[5])]),
                [(0, 8), (8, 8), (16, 8)], '[spec] RGB field positions and sizes, at tag offset 32')
    img = m.screendump('pattern')
    for x, rgb, name in ((32, 0xff0000, 'red'), (96, 0x00ff00, 'green'), (160, 0x0000ff, 'blue'),
                         (224, 0xffffff, 'white')):
        t.expect_eq(img.pixel(x, 32), rgb, f'[spec] the framebuffer described is the one shown: {name} square')


@check(ports=X86_PORTS, tags=['multiboot2'])
def mb2_console(t):
    """Console flags 3 (a console is required, EGA text supported) and no
    framebuffer support declared, by tag or by requesting framebuffer
    information: EGA text on BIOS; UEFI, with no text mode, refuses."""
    m = boot_mb2(t, HostFile(mb2_kernel(t, 'text')))
    if t.port.is_uefi:
        msg, _ = wait_panic(t, m)
        t.expect_eq(msg, 'multiboot2: OS requires text mode, but UEFI does not support it', '[spec] no console')
        return
    r = mb2_report(t, m)
    t.expect_eq((h(r, 'fb.type'), h(r, 'fb.addr'), h(r, 'fb.width'), h(r, 'fb.height'), h(r, 'fb.bpp'),
                 h(r, 'fb.pitch')), (2, 0xb8000, 80, 25, 16, 160), '[spec] EGA text, in characters')
    scr = t.screen(m, 'text')
    t.check(scr.row_text(0).startswith('MB2 TEXT OK'), f'[spec] text memory shown: {scr.row_text(0)!r}')


@check(ports=X86_PORTS, tags=['multiboot2'])
def mb2_no_video_request(t):
    """[intent] With no framebuffer tag, as GRUB does: BIOS gets EGA text;
    UEFI, having no text mode, no mode set and no framebuffer tag."""
    m = boot_mb2(t, HostFile(mb2_kernel(t)))
    r = mb2_report(t, m)
    types = [typ for _, typ, _ in mb2_tags(r)]
    if t.port.is_uefi:
        t.check(8 not in types, 'no framebuffer tag')
    else:
        t.expect_eq(h(r, 'fb.type'), 2, 'EGA text framebuffer tag')


@check(ports=X86_PORTS, tags=['multiboot2'])
def mb2_efi_boot_services(t):
    """[intent] Limine always terminates boot services: an optional EFI boot
    services tag is passed over, a required one refused."""
    m = boot_mb2(t, HostFile(mb2_kernel(t, 'efibs')))
    r = mb2_report(t, m)
    check_mb2_machine_state(t, r)
    m.stop()
    m = boot_mb2(t, HostFile(mb2_kernel(t, 'efibsreq')))
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'multiboot2: EFI_BS tag is required but not supported', 'required EFI boot services tag')
    m.stop()
    # The EFI entry address tags mean nothing without boot services kept, so
    # even required ones are passed over.
    data = bytearray(open(mb2_kernel(t, 'efibs'), 'rb').read())
    for typ in (8, 9):
        at = data.find(struct.pack('<HHI', typ, 1, 12))
        t.require(at >= 0, f'EFI entry address tag {typ} in the header')
        struct.pack_into('<H', data, at + 2, 0)
    m = boot_mb2(t, bytes(data))
    r = mb2_report(t, m)
    check_mb2_machine_state(t, r)


def mb2_header_at(data):
    return data.find(struct.pack('<I', 0xe85250d6))


def mb2_rechecksum(data, at):
    data = bytearray(data)
    magic, arch, length = struct.unpack_from('<III', data, at)
    struct.pack_into('<I', data, at + 12, -(magic + arch + length) & 0xffffffff)
    return bytes(data)


def mb2_info_request_patched(t, variant, first_type):
    """The kernel with its information request's first entry replaced."""
    data = bytearray(open(mb2_kernel(t, variant), 'rb').read())
    at = mb2_header_at(data)
    struct.pack_into('<I', data, at + 16 + 8, first_type)
    return bytes(data)


@check(ports=X86_PORTS, tags=['multiboot2'])
def mb2_information_request(t):
    """[spec] A required request for a tag the loader does not support fails;
    an optional one is passed over."""
    m = boot_mb2(t, mb2_info_request_patched(t, 'elf', 10))
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'multiboot2: Requested tag `10` which is not supported', 'required APM table request')
    m.stop()
    m = boot_mb2(t, mb2_info_request_patched(t, 'infoopt', 10))
    r = mb2_report(t, m)
    t.expect_eq(h(r, 'eax'), MB2_MAGIC, 'optional request passed over')


def mb2_flat(t, offset):
    return open(t.env.payload('multiboot', 'mb2', f'hdr{offset}.bin'), 'rb').read()


@check(ports=X86_PORTS, tags=['multiboot2'])
def mb2_header_placement(t):
    """[spec] The header lies wholly within the first 32768 bytes."""
    m = boot_mb2(t, mb2_flat(t, 32704))
    mb2_report(t, m, 'B MB2-FLAT')
    m.stop()
    for offset, message in ((32712, 'multiboot2: Header extends past the first 32768 bytes'),
                            (32752, 'multiboot2: Header extends past the first 32768 bytes'),
                            (32760, 'multiboot2: Invalid magic'),
                            (32768, 'multiboot2: Invalid magic')):
        m = boot_mb2(t, mb2_flat(t, offset))
        msg, _ = wait_panic(t, m)
        t.expect_eq(msg, message, f'header at {offset}')
        m.stop()


@check(ports=X86_PORTS, tags=['multiboot2'])
def mb2_errors(t):
    hdr = 32704
    base = mb2_flat(t, hdr)
    addr = hdr + 16
    entry_tag = addr + 24
    elf = open(mb2_kernel(t), 'rb').read()
    at = mb2_header_at(elf)
    cases = [
        (None, 'multiboot2: Executable path not specified'),
        (b'\xd6\x50\x52\xe8' + b'\x00' * 8, 'multiboot2: Kernel file too small to contain header'),
        (b'\x00' * 4096, 'multiboot2: Invalid magic'),
        (patched(base, hdr + 12, 0), 'multiboot2: Header checksum is invalid'),
        (mb2_rechecksum(patched(base, hdr + 4, 4), hdr), 'multiboot2: Unsupported architecture 4 (expected i386)'),
        (mb2_rechecksum(patched(elf, at + 8, len(elf)), at), 'multiboot2: Header length exceeds kernel file size'),
        (patched(elf, at + 16 + 4, 0x10000), 'multiboot2: Header tag exceeds header bounds'),
        (patched(elf, at + 16, 0x8000), 'multiboot2: Unknown custom header tag type: 32768'),
        (patched(base, addr + 8, None, KERNEL_BASE + hdr + 8), 'multiboot2: Illegal load address'),
        (patched(base, addr + 8, KERNEL_BASE + hdr, KERNEL_BASE - 8), 'multiboot2: Address tag offset underflow'),
        (patched(base, addr + 8, hdr - 8, 0xffffffff), 'multiboot2: Header offset exceeds header address'),
        (patched(base, addr + 8, None, None, KERNEL_BASE - 1), 'multiboot2: Load end address less than load address'),
        (patched(base, addr + 8, None, None, KERNEL_BASE + 0x100, KERNEL_BASE + 0x80),
         'multiboot2: Illegal bss end address'),
        (patched(base, addr + 8, None, None, KERNEL_BASE + len(base) + 8),
         'multiboot2: load_src + load_size exceeds kernel file size'),
        # The entry address tag made an optional one Limine does not know.
        (patched(base, entry_tag, 0x18000), 'multiboot2: Using address tag but entry address tag missing'),
        (patched(base, entry_tag + 8, KERNEL_BASE + len(base) + 0x1000),
         'multiboot2: Entry point is outside the executable'),
        (patched(base, addr + 8, 0xffff8000 + hdr, 0xffff8000), 'multiboot2: Executable does not fit under 4GiB'),
        (patched(base, addr + 8, 0xf0000000 + hdr, 0xf0000000),
         'multiboot2: Could not find viable load address for executable'),
        (elf[:4] + b'\x03' + elf[5:], 'multiboot2: Invalid ELF file bitness'),
    ]
    for data, message in cases:
        tree = Tree()
        if isinstance(data, bytes):
            tree.add('/boot/kernel', data)
            text = entry('MB2', protocol='multiboot2', path='boot():/boot/kernel')
        else:
            text = entry('MB2', protocol='multiboot2')
        t.limine_files('timeout: 0\n' + text, tree)
        m = t.boot(t.boot_disk(tree))
        msg, _ = wait_panic(t, m)
        t.expect_eq(msg, message, message)
        m.stop()
