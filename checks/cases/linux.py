import os
import struct

from harness.api import check
from harness import efivars
from harness.media import HostFile, Tree
from harness.ports import UEFI_PORTS
from harness.report import Report, fnv1a, unescape
from cases._common import descriptor, entry, wait_panic, wait_done_or_panic

NON_X86 = ['uefi-aarch64', 'uefi-riscv64', 'uefi-loongarch64']
STUB_ARCH = {'uefi-aarch64': 'aarch64', 'uefi-riscv64': 'riscv64', 'uefi-loongarch64': 'loongarch64'}


def stub(t, flavour='default'):
    return HostFile(t.env.payload('linux', STUB_ARCH[t.port.name], flavour, 'Image'))


def boot_linux(t, cmdline='console=ttyS0 root=/dev/vda1 rw', modules=(), extra='', globals_='',
               files=None, flavour='default', **machine):
    tree = Tree()
    tree.add('/Image', stub(t, flavour))
    mods = []
    for path, data in modules:
        tree.add(path, data)
        mods.append((f'boot():{path}', None))
    for path, data in (files or {}).items():
        tree.add(path, data)
    config = 'timeout: 0\n' + globals_ + entry('Linux', protocol='linux', path='boot():/Image',
                                                cmdline=cmdline, modules=mods, extra=extra)
    t.limine_files(config, tree)
    disk = t.boot_disk(tree)
    m = t.boot(disk, **machine)
    return m


def stub_report(t, m):
    m.wait_for('serial', 'E DONE', 60)
    return Report(m.stream('serial').text(), 'B LINUX-STUB')


def initrd_image(modules):
    """What the kernel sees: the modules back to back, each 4-byte padded."""
    out = b''
    for _, data in modules:
        out += data + bytes(-len(data) % 4)
    return out


def check_common(t, r, cmdline, modules):
    """The device tree /chosen and EFI tables every non-x86 Linux boot gets."""
    arch = t.port.arch
    if arch != 'loongarch64':
        t.expect_eq(r['dtb.magic'], 'ok', 'device tree passed')
        t.expect_eq(int(r['dtb.memory_nodes'], 16), 0, 'memory nodes removed')
        t.expect_eq(unescape(r['chosen.bootargs']), cmdline, '/chosen/bootargs')
        t.expect_eq(int(r['chosen.linux,uefi-secure-boot'], 16), 2, 'Secure Boot disabled')
        for prop in ('linux,uefi-system-table', 'linux,uefi-mmap-start', 'linux,uefi-mmap-size',
                     'linux,uefi-mmap-desc-size', 'linux,uefi-mmap-desc-ver'):
            t.check(f'chosen.{prop}' in r, f'/chosen/{prop} present')
        t.expect_eq(int(r['chosen.linux,uefi-mmap-desc-ver'], 16), 1, 'memory map descriptor version')
        t.expect_eq(int(r['chosen.linux,uefi-mmap-size'], 16), int(r['boot_memmap.map_size'], 16),
                    'memory map size agrees with the boot memmap table')
    else:
        t.expect_eq(unescape(r['cmdline']), cmdline, 'command line in a1')
    t.check('efi.signature' in r and int(r['efi.signature'], 16) == 0x5453595320494249,
            'EFI system table reachable')
    tables = r.multi.get('efi.table', [])
    if modules:
        image = initrd_image(modules)
        if arch != 'loongarch64':
            start = int(r['chosen.linux,initrd-start'], 16)
            end = int(r['chosen.linux,initrd-end'], 16)
            t.expect_eq(end - start, len(image), 'initrd size in /chosen')
            t.expect_eq(r['initrd.hash'], '0x' + fnv1a(image).lstrip('0'), 'initrd contents')
            t.expect_eq(int(r['initrd_table.base'], 16), start, 'initrd table base')
        t.expect_eq(int(r['initrd_table.size'], 16), len(image), 'initrd table size')
        t.check(int(r['initrd_table.base'], 16) % 4096 == 0, 'initrd page aligned')
    else:
        t.check('chosen.linux,initrd-start' not in r, 'no initrd in /chosen without modules')
        t.check('5568e427-68fc-4f3d-ac74-ca555231cc68' not in tables, 'no initrd table without modules')
    t.check('800f683f-d08b-423a-a293-965c3c6fe2b4' in tables, 'boot memmap table installed')
    t.expect_eq(int(r['boot_memmap.desc_size'], 16), 48, 'memmap descriptor size')
    t.expect_eq((r['memreserve.capacity'], r['memreserve.count'], r['memreserve.next']),
                ('0x0', '0x0', '0x0'), 'empty memory reservation table')
    t.check('e03fc20a-85dc-406e-b90e-4ab502371d95' in tables, 'screen info table installed')
    t.expect_eq(int(r['screen_info.isvga'], 16), 0x70, 'screen_info: EFI framebuffer')
    t.expect_eq(int(r['screen_info.depth'], 16), 32, 'screen_info depth')


@check(ports=NON_X86, tags=['linux'])
def linux_handoff(t):
    """The handoff Linux Documentation/arch/{arm64,loongarch}/booting.rst and
    riscv/boot.rst ask for, and the tables passed with it."""
    modules = [('/initrd1.img', b'initrd one\n' * 37 + b'x'), ('/initrd2.img', b'second initrd' * 50)]
    cmdline = 'console=ttyS0 root=/dev/vda1 rw quiet'
    m = boot_linux(t, cmdline, modules)
    r = stub_report(t, m)
    arch = t.port.arch
    base = int(r['image_base'], 16)
    if arch == 'aarch64':
        t.check(int(r['x0'], 16) != 0, 'x0 holds the device tree')
        t.expect_eq((r['x1'], r['x2'], r['x3']), ('0x0', '0x0', '0x0'), 'x1-x3 zero')
        t.expect_eq(r['mmu'], '0x0', 'MMU off')
        t.expect_eq(int(r['daif'], 16), 0x3c0, 'all exceptions masked')
        t.expect_eq(int(r['el'], 16), 1, 'entered at EL1')
        t.expect_eq(base % (2 << 20), 0, 'image 2MiB aligned (text_offset 0)')
    elif arch == 'riscv64':
        t.check(int(r['a1'], 16) != 0, 'a1 holds the device tree')
        t.expect_eq(r['satp'], '0x0', 'paging off')
        t.expect_eq((r['sstatus_sie'], r['sie']), ('0x0', '0x0'), 'interrupts off')
        t.expect_eq(base % (2 << 20), 0x200000 % (2 << 20), 'image placed text_offset into a 2MiB block')
    else:
        t.expect_eq(r['a0'], '0x1', 'a0: booted by EFI')
        t.expect_eq(int(r['crmd'], 16) & 4, 0, 'interrupts off')
        t.expect_eq((r['dmw0'], r['dmw1'], r['dmw2'], r['dmw3']),
                    ('0x8000000000000001', '0x9000000000000011', '0xa000000000000021', '0x0'),
                    'DMW0-2 at 0x8, 0x9 and 0xa: uncached, cached, weakly-ordered uncached; DMW3 off')
        t.expect_eq(base % (2 << 20), 0, 'image 2MiB aligned')
    check_common(t, r, cmdline, modules)


@check(ports=NON_X86, tags=['linux'])
def linux_no_modules(t):
    m = boot_linux(t, 'root=/dev/nothing')
    r = stub_report(t, m)
    check_common(t, r, 'root=/dev/nothing', [])


@check(ports=['uefi-aarch64'], tags=['linux'])
def linux_aarch64_el2(t):
    """Firmware at EL2: Linux is entered there, MMU off."""
    m = boot_linux(t, 'x', machine_opts=['virtualization=on'], cpu='max')
    r = stub_report(t, m)
    t.expect_eq(int(r['el'], 16), 2, 'entered at EL2')
    t.expect_eq(r['mmu'], '0x0', 'MMU off')
    check_common(t, r, 'x', [])


@check(ports=['uefi-aarch64'], tags=['linux'])
def linux_aarch64_placement(t):
    """With the header's physical placement flag the image may go anywhere;
    either way it is 2MiB aligned."""
    m = boot_linux(t, 'x', flavour='anywhere', modules=[('/i', b'abc')])
    r = stub_report(t, m)
    t.expect_eq(int(r['image_base'], 16) % (2 << 20), 0, 'image 2MiB aligned')
    check_common(t, r, 'x', [('/i', b'abc')])


@check(ports=NON_X86, tags=['linux', 'hostile'])
def linux_virtual_address_map_refused(t):
    """When SetVirtualAddressMap() fails, here made to by the EFI probe that
    starts Limine, the kernel is booted without runtime services: each
    runtime descriptor it gets has an all-ones virtual address, which kernels
    take for no mapping. A LoongArch kernel uses the runtime services
    regardless, so there Limine refuses to boot instead [intent]."""
    from cases.chainload import probe_then_limine
    m = probe_then_limine(t, 'name:svam svamfail',
                          'timeout: 0\n' + entry('Linux', protocol='linux', path='boot():/Image', cmdline='x'),
                          {'/Image': stub(t)})
    if t.port.name == 'uefi-loongarch64':
        msg, _ = wait_panic(t, m)
        t.check(msg.startswith('linux: failed to set UEFI virtual address map'), f'refused: {msg!r}')
        return
    r = stub_report(t, m)
    t.check('K svamfail=0' in m.stream('serial').text(), 'SetVirtualAddressMap() replaced')
    runtime = int(r['boot_memmap.runtime'], 16)
    t.check(runtime > 0, 'runtime descriptors in the map')
    t.expect_eq(int(r['boot_memmap.runtime_unmapped'], 16), runtime, 'all of them unmapped')


def patched_stub(t, patches, flavour='default'):
    """The stub Image with (offset, format, value) patches to its header."""
    with open(t.env.payload('linux', STUB_ARCH[t.port.name], flavour, 'Image'), 'rb') as f:
        image = bytearray(f.read())
    for off, fmt, value in patches:
        struct.pack_into(fmt, image, off, value)
    return bytes(image)


@check(ports=['uefi-aarch64'], tags=['linux'])
def linux_aarch64_pre_3_17_header(t):
    """[spec] An image_size of zero marks a header from before v3.17, whose
    text_offset is 0x80000 (Linux Documentation/arch/arm64/booting.rst)."""
    m = boot_linux(t, 'x', files={'/Image': patched_stub(t, [(16, '<Q', 0)])})
    r = stub_report(t, m)
    t.expect_eq(int(r['image_base'], 16) % (2 << 20), 0x80000, 'the image 0x80000 into a 2MiB block')


@check(ports=NON_X86, tags=['linux'])
def linux_resolution(t):
    """The entry's resolution option picks the mode the framebuffer described
    to the kernel is in (CONFIG.md)."""
    m = boot_linux(t, 'x', extra='    resolution: 800x600\n')
    r = stub_report(t, m)
    t.expect_eq((int(r['screen_info.width'], 16), int(r['screen_info.height'], 16)), (800, 600),
                'screen_info describes an 800x600 framebuffer')


@check(ports=NON_X86, tags=['linux'])
def linux_errors(t):
    cases = [
        (entry('L', protocol='linux'), 'linux: Kernel path not specified'),
        (entry('L', protocol='linux', path='boot():/nope'),
         'linux: failed to open kernel `boot():/nope`. Is the path correct?'),
        (entry('L', protocol='linux', path='boot():/tiny'),
         'linux: kernel too small to contain a valid header'),
        (entry('L', protocol='linux', path='boot():/garbage'),
         'linux: invalid kernel image: kernel header magic does not match'),
        (entry('L', protocol='linux', path='boot():/Image', modules=[('boot():/missing.img', None)]),
         'linux: failed to open module `boot():/missing.img`. Is the path correct?'),
    ]
    if t.port.arch == 'loongarch64':
        # [intent] The entry the header names must lie in the image.
        cases.append((entry('L', protocol='linux', path='boot():/badentry'),
                      'linux: kernel entry point lies outside the image'))
    if t.port.arch == 'riscv64':
        # [intent] Limine wants header version 0.2 at least.
        cases.append((entry('L', protocol='linux', path='boot():/proto01'),
                      'linux: invalid kernel image: protocols < 0.2 are not supported'))
    for text, message in cases:
        tree = Tree({'/Image': stub(t), '/tiny': b'\x00' * 32, '/garbage': b'\xab' * 4096})
        if t.port.arch == 'riscv64':
            tree.add('/proto01', stub(t, 'proto01'))
        if t.port.arch == 'loongarch64':
            path = t.env.payload('linux', 'loongarch64', 'default', 'Image')
            with open(path, 'rb') as f:
                load_offset = struct.unpack_from('<Q', f.read(32), 24)[0]
            tree.add('/badentry', patched_stub(t, [(8, '<Q', load_offset + os.path.getsize(path) + 0x1000)]))
        t.limine_files('timeout: 0\n' + text, tree)
        m = t.boot(t.boot_disk(tree))
        msg, _ = wait_panic(t, m)
        t.expect_eq(msg, message, text.split('\n')[2].strip() if '\n' in text else text)
        m.stop()


# x86: a bzImage stand-in reporting registers and the zero page on port 0xe9.
# Expectations marked [spec] come from Linux Documentation/arch/x86/boot.rst
# (boot protocol 2.15) and zero-page.rst, [intent] ones are choices Limine's
# code states.

X86 = ['bios', 'uefi-ia32', 'uefi-x86-64']
PREF_ADDRESS = 0x1000000
KERNEL_ALIGNMENT = 0x200000
INIT_SIZE = 0x40000


def bzimage(t, variant='default'):
    return HostFile(t.env.payload('linux', 'x86', variant, 'bzImage'))


def patched_bzimage(t, patches, variant='default'):
    """The default bzImage with bytes replaced: {offset: bytes}."""
    data = bytearray(open(t.env.payload('linux', 'x86', variant, 'bzImage'), 'rb').read())
    for off, b in patches.items():
        data[off:off + len(b)] = b
    return bytes(data)


def boot_bzimage(t, variant='default', cmdline='console=ttyS0 root=/dev/sda1', modules=(), extra='',
                 files=None, **machine):
    tree = Tree()
    tree.add('/bzImage', bzimage(t, variant))
    mods = []
    for path, data in modules:
        tree.add(path, data)
        mods.append((f'boot():{path}', None))
    for path, data in (files or {}).items():
        tree.add(path, data)
    config = 'timeout: 0\n' + entry('Linux', protocol='linux', path='boot():/bzImage',
                                    cmdline=cmdline, modules=mods, extra=extra)
    t.limine_files(config, tree)
    return t.boot(t.boot_disk(tree), **machine)


def x86_report(t, m):
    m.wait_for('debugcon', 'E DONE', 60)
    return Report(m.stream('debugcon').text(), 'B LINUX-X86-STUB')


def hexint(r, key):
    return int(r[key], 16)


def check_flat_segments(t, r):
    """[spec] __BOOT_CS (0x10) and __BOOT_DS (0x18): 4G flat, execute/read
    and read/write."""
    cs, ds = descriptor(hexint(r, 'gdt.10')), descriptor(hexint(r, 'gdt.18'))
    for name, d in (('__BOOT_CS', cs), ('__BOOT_DS', ds)):
        t.expect_eq((d['present'], d['s'], d['base'], d['limit']), (1, 1, 0, 0xffffffff),
                    f'[spec] {name} present, flat, 4GiB')
    t.expect_eq(cs['type'] & 0b1010, 0b1010,
                '[spec] __BOOT_CS execute/read (Intel SDM 325462-093 vol. 3A, table 3-1)')
    t.expect_eq(ds['type'] & 0b1110, 0b0010,
                '[spec] __BOOT_DS read/write, expand-up (Intel SDM 325462-093 vol. 3A, table 3-1)')
    t.expect_eq(hexint(r, 'reg.cs'), 0x10, '[spec] CS is __BOOT_CS')
    t.expect_eq([hexint(r, f'reg.{s}') for s in ('ds', 'es', 'ss')], [0x18] * 3, '[spec] DS, ES, SS are __BOOT_DS')
    t.expect_eq(hexint(r, 'reg.eflags') & 0x200, 0, '[spec] interrupts disabled')


def check_zero_page(t, r, cmdline, modules):
    t.expect_eq(hexint(r, 'bp.sentinel'), 0, '[spec] zero page cleared before the header is copied in')
    t.expect_eq(hexint(r, 'bp.type_of_loader'), 0xff, '[spec] type_of_loader: undefined loader')
    t.expect_eq(hexint(r, 'bp.loadflags'), 0x01, '[spec] loadflags: LOADED_HIGH kept; [intent] QUIET_FLAG cleared')
    t.expect_eq(hexint(r, 'bp.vid_mode'), 0xffff, '[spec] vid_mode: normal')
    t.expect_eq(unescape(r['cmdline']), cmdline, '[spec] command line')
    load = hexint(r, 'load_address')
    if load <= 0xffffffff:
        t.expect_eq(hexint(r, 'bp.code32_start'), load, '[spec] code32_start points at the loaded kernel')
    init_end = load + hexint(r, 'bp.init_size')

    def outside_init_window(start, size, what):
        t.check(start + size <= load or start >= init_end,
                f'[spec] {what} {start:#x}+{size:#x} clear of the init_size window {load:#x}-{init_end:#x}')

    outside_init_window(hexint(r, 'reg.esi'), 4096, 'zero page')
    outside_init_window(hexint(r, 'bp.cmd_line_ptr'), len(cmdline.encode()) + 1, 'command line')
    if modules:
        image = initrd_image(modules)
        t.expect_eq(hexint(r, 'bp.ramdisk_size'), len(image),
                    '[intent] ramdisk size: modules back to back, 4-byte padded')
        t.expect_eq(r['ramdisk.hash'], '0x' + fnv1a(image).lstrip('0'), '[spec] ramdisk contents')
        start = hexint(r, 'bp.ramdisk_image')
        t.expect_eq(start % 0x100000, 0, '[intent] ramdisk 1MiB aligned')
        t.check(start + len(image) - 1 <= hexint(r, 'bp.initrd_addr_max'),
                f'[spec] ramdisk {start:#x}+{len(image):#x} within initrd_addr_max')
        outside_init_window(start, len(image), 'ramdisk')
    else:
        t.expect_eq((hexint(r, 'bp.ramdisk_image'), hexint(r, 'bp.ramdisk_size')), (0, 0), '[spec] no ramdisk')
    t.check(hexint(r, 'bp.acpi_rsdp_addr') != 0, '[spec] RSDP address given')
    t.expect_eq(hexint(r, 'rsdp.signature'), int.from_bytes(b'RSD PTR ', 'little'),
                '[spec] RSDP signature at that address')
    t.check(hexint(r, 'e820.count') > 0, '[spec] e820 entries')
    entries = [tuple(int(x, 16) for x in v.split()) for v in r.multi['e820']]
    t.check(all(e[2] in (1, 2, 3, 4, 5) for e in entries), f'e820 types valid: {entries}')
    if t.port.is_uefi:
        t.expect_eq(hexint(r, 'si.isvga'), 0x70, '[spec] screen_info: VIDEO_TYPE_EFI')
        t.expect_eq(hexint(r, 'si.size'), hexint(r, 'si.linelength') * hexint(r, 'si.height'),
                    '[spec] lfb_size in bytes (efifb)')
        sig = b'EL64' if t.port.name == 'uefi-x86-64' else b'EL32'
        t.expect_eq(hexint(r, 'efi.loader_signature'), int.from_bytes(sig, 'little'), '[spec] efi_loader_signature')
        t.check(hexint(r, 'efi.systab') != 0 and hexint(r, 'efi.memmap') != 0,
                '[spec] EFI system table and map given')
        t.expect_eq(hexint(r, 'efi.memdesc_version'), 1, '[spec] memory descriptor version (UEFI 2.11)')
        t.expect_eq(hexint(r, 'bp.secure_boot'), 2, '[intent] secure_boot: disabled')
    else:
        t.expect_eq(hexint(r, 'si.isvga'), 0x23, '[spec] screen_info: VIDEO_TYPE_VLFB')
        lfb = hexint(r, 'si.linelength') * hexint(r, 'si.height')
        t.expect_eq(hexint(r, 'si.size'), -(-lfb // 65536), '[spec] lfb_size in 64KiB units (vesafb)')
        t.expect_eq(hexint(r, 'efi.loader_signature'), 0, '[spec] no EFI on BIOS')
    t.expect_eq(hexint(r, 'si.depth'), 32, '[intent] framebuffer depth')
    t.expect_eq(hexint(r, 'si.capabilities'), 2, '[intent] VIDEO_CAPABILITY_64BIT_BASE, quirks not skipped')
    t.expect_eq(hexint(r, 'si.flags'), 1, '[intent] VIDEO_FLAGS_NOCURSOR')


def check_relocated(t, r):
    """[spec] A relocatable kernel goes somewhere kernel_alignment allows;
    where pref_address is free, there. [intent] Never below pref_address."""
    load = hexint(r, 'load_address')
    t.expect_eq(load % KERNEL_ALIGNMENT, 0, f'[spec] load address {load:#x} kernel_alignment aligned')
    if t.port.is_uefi:
        t.check(load >= PREF_ADDRESS, f'[intent] load address {load:#x} not below pref_address')
    else:
        t.expect_eq(load, PREF_ADDRESS, '[spec] loaded at pref_address, which is free on BIOS')


@check(ports=X86, tags=['linux'])
def linux_x86_32bit_entry(t):
    """The 32-bit boot protocol: flat 4GiB __BOOT_CS/__BOOT_DS, interrupts off,
    paging off, %esi the zero page, %ebp, %edi and %ebx zero."""
    modules = [('/initrd.img', b'initramfs contents ' * 100 + b'z'), ('/ucode.img', b'microcode' * 7)]
    cmdline = 'console=ttyS0,115200 root=/dev/sda2 ro'
    m = boot_bzimage(t, cmdline=cmdline, modules=modules)
    r = x86_report(t, m)
    t.check('K mode=32' in r.raw, '[spec] entered through the 32-bit entry point')
    check_relocated(t, r)
    t.check(hexint(r, 'reg.esi') != 0, '[spec] esi holds the zero page')
    t.expect_eq((hexint(r, 'reg.ebp'), hexint(r, 'reg.edi'), hexint(r, 'reg.ebx')), (0, 0, 0),
                '[spec] ebp, edi, ebx zero')
    check_flat_segments(t, r)
    t.expect_eq([hexint(r, f'reg.{s}') for s in ('fs', 'gs')], [0x18] * 2, '[intent] fs, gs are __BOOT_DS too')
    t.expect_eq(hexint(r, 'reg.eflags') & 0x400, 0, '[intent] direction flag clear')
    t.expect_eq(hexint(r, 'reg.cr0') & 0x80000001, 1, '[spec] protected mode, paging off')
    check_zero_page(t, r, cmdline, modules)


@check(ports=['uefi-x86-64'], tags=['linux'])
def linux_x86_64bit_entry(t):
    """A kernel that can load above 4GiB and has a 64-bit entry is entered
    there, in long mode, when it is loaded above 4GiB."""
    m = boot_bzimage(t, 'above4g', cmdline='x', modules=[('/initrd.img', b'hi there')], memory='6G')
    r = x86_report(t, m)
    t.check('K mode=64' in r.raw, '[intent] entered through the 64-bit entry point when above 4GiB')
    load = hexint(r, 'load_address')
    t.expect_eq(load, 0x100000000, '[spec] loaded at pref_address, which is free')
    check_flat_segments(t, r)
    t.expect_eq((hexint(r, 'reg.ebp'), hexint(r, 'reg.edi'), hexint(r, 'reg.ebx')), (0, 0, 0),
                '[intent] ebp, edi, ebx zero in 64-bit mode too')
    t.check(hexint(r, 'reg.cr0') & 0x80000000, '[spec] paging on')
    t.check(hexint(r, 'reg.efer') & 0x400, '[spec] 64-bit mode (EFER.LMA)')
    t.expect_eq(descriptor(hexint(r, 'gdt.10'))['l'], 1, '[spec] __BOOT_CS a 64-bit code segment')
    for what in ('kernel', 'zero_page', 'cmdline'):
        t.expect_eq(hexint(r, f'idmap.{what}'), 0xffffffffffffffff, f'[spec] {what} identity mapped')
    check_zero_page(t, r, 'x', [('/initrd.img', b'hi there')])


@check(ports=X86, tags=['linux'])
def linux_x86_non_relocatable(t):
    """[intent] A non-relocatable kernel goes at pref_address, where boot
    protocol 2.15 says it runs, or not at all."""
    m = boot_bzimage(t, 'nonreloc')
    what, msg = wait_done_or_panic(t, m, 'debugcon')
    if what == 'panic':
        t.require(t.port.is_uefi, f'panicked on BIOS, where pref_address is free: {msg}')
        t.expect_eq(msg, 'linux: Non-relocatable kernel could not be loaded at required address 0x1000000',
                    'pref_address taken by the firmware')
        return
    r = x86_report(t, m)
    t.expect_eq(hexint(r, 'load_address'), PREF_ADDRESS, 'loaded at pref_address')
    check_zero_page(t, r, 'console=ttyS0 root=/dev/sda1', [])


@check(ports=X86, tags=['linux'])
def linux_x86_pre210_non_relocatable(t):
    """[spec] A non-relocatable kernel from before pref_address (2.10) goes at
    0x100000; its kernel_alignment, 2MiB here, binds relocatable ones only."""
    m = boot_bzimage(t, 'proto209')
    what, msg = wait_done_or_panic(t, m, 'debugcon')
    if what == 'panic':
        t.require(t.port.is_uefi, f'panicked on BIOS, where 0x100000 is free: {msg}')
        t.expect_eq(msg, 'linux: Non-relocatable kernel could not be loaded at required address 0x100000',
                    '0x100000 taken by the firmware')
        return
    r = x86_report(t, m)
    t.expect_eq(hexint(r, 'load_address'), 0x100000, 'loaded at 0x100000')
    t.expect_eq(hexint(r, 'bp.code32_start'), 0x100000, 'code32_start points at it')


@check(ports=X86, tags=['linux'])
def linux_x86_initrd_addr_max(t):
    """[spec] The initrd stays under the kernel's initrd_addr_max."""
    m = boot_bzimage(t, 'lowinitrd', modules=[('/i', b'q' * 300000)])
    r = x86_report(t, m)
    t.check(hexint(r, 'bp.ramdisk_image') + 300000 - 1 <= 0x07ffffff, f'ramdisk at {r["bp.ramdisk_image"]}')


@check(ports=['bios'], tags=['linux'])
def linux_x86_textmode(t):
    """textmode: yes hands over VGA text mode 3 (BIOS only)."""
    m = boot_bzimage(t, extra='textmode: yes')
    r = x86_report(t, m)
    t.expect_eq(hexint(r, 'si.isvga'), 0x22, '[spec] screen_info: VIDEO_TYPE_VGAC')
    t.expect_eq((hexint(r, 'si.orig_video_mode'), hexint(r, 'si.orig_video_cols'),
                 hexint(r, 'si.orig_video_lines')), (3, 80, 25), '[intent] mode 3, 80x25')


@check(ports=X86, tags=['linux'])
def linux_x86_resolution(t):
    m = boot_bzimage(t, extra='resolution: 800x600')
    r = x86_report(t, m)
    t.expect_eq((hexint(r, 'si.width'), hexint(r, 'si.height')), (800, 600), 'framebuffer resolution')


def u8(v):
    return bytes([v])


def u32(v):
    return v.to_bytes(4, 'little')


@check(ports=X86, tags=['linux'])
def linux_x86_errors(t):
    big_setup = {0x1f1: u8(0x10)}
    cases = [
        (None, None, 'linux: Kernel path not specified'),
        ('/nope', None, 'linux: Failed to open kernel with path `boot():/nope`. Is the path correct?'),
        ('/small', b'\x00' * 0x205, 'linux: Kernel file too small'),
        ('/nosig', b'\x00' * 4096, 'linux: Invalid kernel signature'),
        ('/shortsetup', lambda: patched_bzimage(t, big_setup)[:0x1000],
         'linux: Kernel file too small for real mode code'),
        ('/longheader', lambda: patched_bzimage(t, {0x201: u8(0x90)}),
         'linux: Setup header too long for the zero page'),
        ('/proto201', lambda: open(t.env.payload('linux', 'x86', 'proto201', 'bzImage'), 'rb').read(),
         'linux: Protocols < 2.02 are not supported'),
        ('/nohigh', lambda: open(t.env.payload('linux', 'x86', 'nohigh', 'bzImage'), 'rb').read(),
         'linux: Kernels that load at 0x10000 are not supported'),
        ('/align', lambda: patched_bzimage(t, {0x230: u32(0x300000)}),
         'linux: kernel_alignment is not a power of two'),
        ('/huge', lambda: patched_bzimage(t, {0x260: u32(0x80000000)}),
         'linux: Failed to allocate memory for kernel'),
        ('/pinned', lambda: patched_bzimage(t, {0x258: (0x40000000).to_bytes(8, 'little')}, 'nonreloc'),
         'linux: Non-relocatable kernel could not be loaded at required address 0x40000000'),
    ]
    for path, data, message in cases:
        tree = Tree({'/bzImage': bzimage(t)})
        if data is not None:
            tree.add(path, data() if callable(data) else data)
        text = entry('L', protocol='linux', path=f'boot():{path}' if path else None)
        if path is None:
            text = entry('L', protocol='linux')
        t.limine_files('timeout: 0\n' + text, tree)
        m = t.boot(t.boot_disk(tree))
        msg, _ = wait_panic(t, m)
        t.expect_eq(msg, message, path or 'no path')
        m.stop()
    tree = Tree({'/bzImage': bzimage(t)})
    t.limine_files('timeout: 0\n' + entry('L', protocol='linux', path='boot():/bzImage',
                                          modules=[('boot():/missing.img', None)]), tree)
    m = t.boot(t.boot_disk(tree))
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'linux: Failed to open module with path `boot():/missing.img`. Is the path correct?',
                'missing module')


SEED_GUID = '1ce1e5bc-7ceb-42f2-81e5-8aadf180f57b'


@check(ports=UEFI_PORTS, tags=['linux', 'uefi'])
def linux_random_seed(t):
    """The random seed table (LINUX_EFI_RANDOM_SEED_TABLE_GUID), filled from
    the firmware's RNG, as without the RandomSeed variable, and from the
    variable's bytes, which is deleted before they are used, or deleted unread
    past 1024 bytes; an earlier stage's table carried over into it [intent]."""
    from cases.chainload import probe_then_limine
    path, kernel = ('/bzImage', bzimage(t)) if t.port.is_x86 else ('/Image', stub(t))
    config = 'timeout: 0\n' + entry('Linux', protocol='linux', path=f'boot():{path}', cmdline='x')

    def table(seed=None, carried=0):
        """The seed table's size, and whether the variable outlived the boot."""
        t.counter += 1
        store = efivars.store(t.env.ovmf(t.port.vars), t.path(f'vars{t.counter}.fd'),
                              [] if seed is None else [('RandomSeed', SEED_GUID, seed)])
        if carried:
            m = probe_then_limine(t, f'name:seed seedtable:{carried:x}', config, {path: kernel}, vars_file=store)
        else:
            tree = Tree()
            tree.add(path, kernel)
            t.limine_files(config, tree)
            m = t.boot(t.boot_disk(tree), vars_file=store)
        r = x86_report(t, m) if t.port.is_x86 else stub_report(t, m)
        size = int(r['rng_seed.size'], 16) if 'rng_seed.size' in r else 0
        m.stop()
        return size, ('RandomSeed', SEED_GUID) in efivars.read(m.workdir + '/vars.fd')

    rng, _ = table()
    for what, seed, carried, more in (('a seed', bytes(range(64)), 0, 64), ('an oversized seed', bytes(2000), 0, 0),
                                      ('an earlier table', bytes(range(64)), 64, 128)):
        size, left = table(seed, carried)
        t.expect_eq(size - rng, more, f'{what}: the table {more} bytes over the RNG\'s {rng}')
        t.check(not left, f'{what}: the variable deleted')
