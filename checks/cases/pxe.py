import ipaddress
import time

from harness.api import check
from harness.media import HostFile, Tree
from harness.ports import ALL_PORTS, UEFI_PORTS
from harness.report import unescape
from cases._common import (check_file, check_report_clean, entry, kernel_file, panics_in_turn, read, screen_text,
                           wait_panic)
from cases.multiboot import mb2_kernel, mb2_report, mb2_tags

# Booting over the network, as USAGE.md's BIOS/PXE and UEFI/PXE sections and
# CONFIG.md's boot and tftp resources describe it, from the DHCP and TFTP
# servers of QEMU's user networking. They hand out 10.0.2.15 and answer from
# 10.0.2.2, and the TFTP server reports sizes (RFC 2349).

CLIENT, SERVER = '10.0.2.15', '10.0.2.2'
MEDIA_TFTP = 2
DHCPACK = 5


def pxe_tree(t, configs, files=None):
    """What the server holds: the boot file, for BIOS limine-bios.sys, and
    configs and files by path."""
    tree = Tree()
    if t.port.is_uefi:
        tree.add('/' + t.port.efi_name, HostFile(t.env.bin(t.port.efi_name)))
    else:
        tree.add('/limine-bios-pxe.bin', HostFile(t.env.bin('limine-bios-pxe.bin')))
        tree.add('/limine-bios.sys', HostFile(t.env.bin('limine-bios.sys')))
    for path, data in {**configs, **(files or {})}.items():
        tree.add(path, data)
    return tree


def boot_file(t):
    return t.port.efi_name if t.port.is_uefi else 'limine-bios-pxe.bin'


def pxe_boot(t, configs, files=None, **machine):
    """Boot Limine over PXE; configs maps paths on the server to configs."""
    return t.boot_pxe(pxe_tree(t, configs, files), boot_file(t), **machine)


def kernel_entry(t, cmdline, path='boot():/boot/kernel.elf', modules=()):
    return entry('Net', path=path, cmdline=cmdline, modules=modules)


def dhcp_message(packet):
    """(op, yiaddr, magic cookie, options) of a DHCP message (RFC 2131,
    section 2; options per RFC 2132)."""
    options, i = {}, 240
    while i < len(packet) and packet[i] != 255:
        if packet[i] == 0:
            i += 1
            continue
        options[packet[i]] = packet[i + 2:i + 2 + packet[i + 1]]
        i += 2 + packet[i + 1]
    return packet[0], str(ipaddress.IPv4Address(packet[16:20])), packet[236:240].hex(), options


@check(ports=ALL_PORTS, tags=['pxe'])
def pxe_boot_files(t):
    """Booted over PXE, Limine reads its config and what it loads from the
    server: boot() standing for tftp(), tftp() for the server it booted from,
    tftp(ip) for the one given. The files are TFTP media, their server's
    address and port given (PROTOCOL.md)."""
    kernel = read(kernel_file(t, 'full'))
    m1, m2 = b'first module\n' * 50, b'second module\n' * 70
    config = 'timeout: 0\n' + kernel_entry(t, 'over the net', modules=[
        ('tftp():/boot/m1.bin', 'one'), (f'tftp({SERVER}):/m2.bin', 'two')])
    m = pxe_boot(t, {'/limine.conf': config},
                 {'/boot/kernel.elf': kernel, '/boot/m1.bin': m1, '/m2.bin': m2})
    r = t.kernel_report(m, timeout=120)
    check_report_clean(t, r)
    t.expect_eq(unescape(r['cmdline']), 'over the net', 'booted')
    check_file(t, r, 'executable_file', path='/boot/kernel.elf', data=kernel, string='over the net',
               partition=0, media_type=MEDIA_TFTP)
    check_file(t, r, 'module.00', path='/boot/m1.bin', data=m1, string='one', partition=0,
               media_type=MEDIA_TFTP)
    check_file(t, r, 'module.01', path='/m2.bin', data=m2, string='two', partition=0, media_type=MEDIA_TFTP)
    for prefix in ('executable_file', 'module.00', 'module.01'):
        t.expect_eq(r[prefix + '.tftp'], f'{SERVER}:69', f'[spec] {prefix} server')


@check(ports=ALL_PORTS, tags=['pxe'])
def pxe_config_search(t):
    """The config is looked for on the server as on any volume (CONFIG.md):
    on UEFI /EFI/BOOT/limine.conf first, the image's path being no file
    path, then /boot/limine/limine.conf, and /limine.conf last."""
    configs = {'/limine.conf': 'timeout: 0\n' + kernel_entry(t, 'root'),
               '/boot/limine/limine.conf': 'timeout: 0\n' + kernel_entry(t, 'boot limine')}
    expected = 'boot limine'
    if t.port.is_uefi:
        configs['/EFI/BOOT/limine.conf'] = 'timeout: 0\n' + kernel_entry(t, 'efi boot')
        expected = 'efi boot'
    m = pxe_boot(t, configs, {'/boot/kernel.elf': HostFile(kernel_file(t, 'full'))})
    t.expect_eq(unescape(t.kernel_report(m, timeout=120)['cmdline']), expected, '[spec] the config used')


@check(ports=UEFI_PORTS, tags=['pxe', 'uefi'])
def pxe_no_uefi_shell(t):
    """Booted over PXE the menu offers no UEFI Shell, even with one on the
    server (USAGE.md)."""
    config = 'timeout: no\n' + kernel_entry(t, 'menu')
    m = pxe_boot(t, {'/limine.conf': config}, {'/' + t.port.shell_name: HostFile(t.env.bin(t.port.efi_name)),
                                               '/boot/kernel.elf': HostFile(kernel_file(t, 'full'))})
    text = screen_text(t.wait_text(m, 'ARROWS', 120))
    t.check('UEFI Shell' not in text, f'[spec] no U key:\n{text}')


@check(ports=['bios', 'uefi-ia32', 'uefi-x86-64'], tags=['pxe', 'multiboot'])
def pxe_mb2_network_tag(t):
    """A Multiboot 2 kernel booted over PXE gets the DHCP ACK in a network
    tag (Multiboot2 2.0 section 3.6.18), as GRUB passes it."""
    config = 'timeout: 0\n' + entry('MB2', protocol='multiboot2', path='boot():/boot/kernel', cmdline='net')
    m = pxe_boot(t, {'/limine.conf': config}, {'/boot/kernel': HostFile(mb2_kernel(t))})
    r = mb2_report(t, m)
    t.expect_eq([tag[1] for tag in mb2_tags(r)].count(16), 1, '[spec] one network tag')
    t.require('network' in r, 'network tag reported')
    op, yiaddr, cookie, options = dhcp_message(bytes.fromhex(r['network']))
    t.expect_eq((op, yiaddr, cookie), (2, CLIENT, '63825363'), '[spec: RFC 2131] a BOOTREPLY to us, DHCP magic cookie')
    t.expect_eq(options.get(53), bytes([DHCPACK]), '[spec: RFC 2132] message type DHCPACK')


@check(ports=ALL_PORTS, tags=['pxe'])
def pxe_tftp_off_pxe(t):
    """Off PXE the tftp resource is unavailable (CONFIG.md): the file is
    not found, with or without a server named."""
    for path in ('tftp():/boot/kernel.elf', f'tftp({SERVER}):/boot/kernel.elf'):
        tree = Tree()
        tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
        t.limine_files('timeout: 0\n' + kernel_entry(t, 'disk', path=path), tree)
        m = t.boot(t.boot_disk(tree))
        msg, _ = wait_panic(t, m)
        t.check('Failed to open' in msg or 'not found' in msg.lower(), f'[spec] {path}: {msg}')
        m.stop()


def boot_outcome(t, m, timeout=120):
    """'booted', 'no config' if Limine's menu says it found none, or None."""
    deadline = time.monotonic() + timeout * t.port.timeout_scale
    while time.monotonic() < deadline:
        if 'E DONE' in m.stream(t.kernel_channel).text():
            return 'booted'
        try:
            if '[config file not found]' in screen_text(t.screen(m)):
                return 'no config'
        except Exception:
            pass
        time.sleep(1)
    return None


@check(ports=ALL_PORTS, tags=['pxe'])
def pxe_tsize(t):
    """USAGE.md: the server must report sizes (tsize, RFC 2349), Limine
    asking for the size of every file and taking a file whose size goes
    unreported for missing; on UEFI through the firmware, which EDK2 sizes
    files with only by tsize, though UEFI 2.11 has it download them to learn
    their size (EFI_PXE_BASE_CODE_PROTOCOL.Mtftp())."""
    config = 'timeout: 0\n' + kernel_entry(t, 'sized')
    files = {'/boot/kernel.elf': HostFile(kernel_file(t, 'full'))}

    def boot(options):
        # DHCP tells the firmware the size of the boot file, which it would
        # not otherwise fetch without tsize.
        return t.boot_pxe(pxe_tree(t, {'/limine.conf': config}, files), boot_file(t), tftp_options=options,
                          bootfile_size=t.port.is_uefi)

    m = boot(('blksize', 'tsize'))
    t.expect_eq(unescape(t.kernel_report(m, timeout=120)['cmdline']), 'sized', 'a server with tsize boots')
    t.check(not m.boot_server.errors, f'server errors: {m.boot_server.errors}')
    if not t.port.is_uefi:
        requests = m.boot_server.requests
        for name in ('limine-bios.sys', 'limine.conf', 'boot/kernel.elf'):
            asked = [opts for n, opts in requests if n.lstrip('/').endswith(name)]
            t.check(any('tsize' in opts for opts in asked), f'[spec] the size of {name} asked: {requests}')
    m.stop()
    # Without tsize: a server with other options, and one with none at all.
    for options in (('blksize',), ()):
        m = boot(options)
        if t.port.is_uefi:
            t.expect_eq(boot_outcome(t, m), 'no config', f'[spec] options {options}: limine.conf, its size '
                        'unreported, missing')
        else:
            msg, scr = wait_panic(t, m, timeout=120)
            t.check('Stage 3 file not found' in screen_text(scr) and 'Failed to load stage 3' in msg,
                    f'[spec] options {options}: limine-bios.sys, its size unreported, missing: {msg}')
        m.stop()


@check(ports=ALL_PORTS, tags=['pxe', 'hostile'])
def pxe_tftp_errors(t):
    """A tftp server address that is no IPv4 address is refused; a file the
    server does not have, or, on BIOS, one whose name is past the 127
    characters the PXE API takes, fails to open; each time the menu comes
    back [intent]."""
    long_name = '/' + 'n' * 130
    cases = [('tftp(10.0.2.300):/m.bin', 'tftp: Invalid ipv4 address: 10.0.2.300'), ('tftp():/missing.bin', None)]
    if not t.port.is_uefi:
        cases.append((f'tftp():{long_name}', None))
    config = 'timeout: no\n' + ''.join(
        entry(f'E{i}', path='boot():/boot/kernel.elf', cmdline='x', modules=[(path, None)])
        for i, (path, _) in enumerate(cases))
    m = pxe_boot(t, {'/limine.conf': config}, {'/boot/kernel.elf': HostFile(kernel_file(t, 'full')),
                                               '/m.bin': b'module\n', long_name: b'long\n'})
    for (path, want), got in zip(cases, panics_in_turn(t, m, len(cases), timeout=120)):
        want = want or f'limine: Failed to open module with path `{path}`. Is the path correct?'
        t.expect_eq(got, want, path)
