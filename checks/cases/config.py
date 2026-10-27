from harness.api import check
from harness.media import Disk, Fat, HostFile, Part, Tree, ESP_TYPE, BASIC_DATA_TYPE
from harness.ports import ALL_PORTS, UEFI_PORTS
from harness import media
from harness.report import unescape
from cases._common import entry, kernel_file, wait_panic

# The configuration file: where it is found and how it is read. Expectations
# come from CONFIG.md unless marked [intent].

ARCH = {'bios': 'x86-64', 'uefi-ia32': 'x86-64', 'uefi-x86-64': 'x86-64', 'uefi-aarch64': 'aarch64',
        'uefi-riscv64': 'riscv64', 'uefi-loongarch64': 'loongarch64'}
LOADER_ARCH = {'bios': 'ia-32', 'uefi-ia32': 'ia-32', 'uefi-x86-64': 'x86-64', 'uefi-aarch64': 'aarch64',
               'uefi-riscv64': 'riscv64', 'uefi-loongarch64': 'loongarch64'}


SMBIOS_GUID = 'eb9d2d31-2d88-11d3-9a16-0090273fc14d'


def kernel_config(cmdline, title='Test', extra='', path='boot():/boot/kernel.elf', globals_='timeout: 0\n'):
    return globals_ + entry(title, path=path, cmdline=cmdline, extra=extra)


def with_kernel(t, tree=None):
    tree = tree or Tree()
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
    return t.limine_files(None, tree)


def booted_cmdline(t, m, timeout=60):
    return unescape(t.kernel_report(m, timeout=timeout)['cmdline'])


def boot_config(t, config, files=None, **machine):
    """Boot a disk with config at /boot/limine.conf; the cmdline booted."""
    tree = with_kernel(t)
    tree.add('/boot/limine.conf', config)
    for path, data in (files or {}).items():
        tree.add(path, data)
    m = t.boot(t.boot_disk(tree), **machine)
    return booted_cmdline(t, m), m


def candidates(t):
    names = ['/boot/limine/limine.conf', '/boot/limine.conf', '/limine/limine.conf', '/limine.conf']
    return (['/EFI/BOOT/limine.conf'] if t.port.is_uefi else []) + names


@check(ports=ALL_PORTS, tags=['config'])
def config_search_order(t):
    """Each candidate location is used when it is the first one present."""
    names = candidates(t)
    for i, first in enumerate(names):
        tree = with_kernel(t)
        for name in names[i:]:
            tree.add(name, kernel_config(f'config at {name}'))
        m = t.boot(t.boot_disk(tree))
        t.expect_eq(booted_cmdline(t, m), f'config at {first}', f'with candidates from {first} on')
        m.stop()


@check(ports=UEFI_PORTS, tags=['config'])
def config_efi_app_directory(t):
    """On EFI, <EFI app path>/limine.conf comes first, and /EFI/BOOT/limine.conf
    is only tried in its stead where that path cannot be determined."""
    efi_name = t.port.efi_name
    arch = {'uefi-ia32': 'ia32', 'uefi-x86-64': 'x86_64'}.get(t.port.name, t.port.arch)
    probe = HostFile(t.env.payload('efi', arch, 'efiprobe.efi'))
    for with_app_config, want in ((True, 'app directory'), (False, 'boot directory')):
        tree = Tree()
        tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
        tree.add('/EFI/BOOT/' + efi_name, probe)
        tree.add('/efiprobe.cmd', f'name:outer chain:\\EFI\\custom\\{efi_name}\n')
        tree.add('/EFI/custom/' + efi_name, HostFile(t.env.bin(efi_name)))
        if with_app_config:
            tree.add('/EFI/custom/limine.conf', kernel_config('app directory'))
        tree.add('/EFI/BOOT/limine.conf', kernel_config('EFI/BOOT'))
        tree.add('/boot/limine.conf', kernel_config('boot directory'))
        m = t.boot(t.boot_disk(tree))
        t.expect_eq(booted_cmdline(t, m), want, f'app directory config {"present" if with_app_config else "absent"}')
        m.stop()


def two_partition_disk(t, first, second):
    """A disk with two FAT partitions, boot code on the second on UEFI (its
    ESP) and installed for BIOS."""
    if t.port.is_uefi:
        parts = [Part(size_mb=20, fs=Fat(16, files=first), type=BASIC_DATA_TYPE),
                 Part(size_mb=20, fs=Fat(16, files=second), type=ESP_TYPE)]
        disk = Disk('gpt', 48, parts=parts)
    else:
        parts = [Part(size_mb=20, fs=Fat(16, files=first), type='0e', bootable=True),
                 Part(size_mb=20, fs=Fat(16, files=second), type='0e')]
        disk = Disk('mbr', 48, parts=parts)
    path = t.path(f'twopart{t.counter}.img')
    t.counter += 1
    disk.build(path, t.workdir)
    if not t.port.is_uefi:
        media.bios_install(t.env, path)
    disk.path = path
    return disk


@check(ports=ALL_PORTS, tags=['config'])
def config_partition_order(t):
    """On EFI the volume holding Limine is searched first; then, and on BIOS,
    the partitions in order."""
    first = with_kernel(t, Tree())
    second = with_kernel(t, Tree())
    first.add('/limine.conf', kernel_config('partition 1'))
    second.add('/limine.conf', kernel_config('partition 2'))
    if t.port.is_uefi:
        # limine_files() put the EFI executable in both; it boots from the ESP.
        first.files.pop('/EFI/BOOT/' + t.port.efi_name)
    m = t.boot(two_partition_disk(t, first, second))
    t.expect_eq(booted_cmdline(t, m), 'partition 2' if t.port.is_uefi else 'partition 1', 'config chosen')
    m.stop()
    if t.port.is_uefi:
        second.files.pop('/limine.conf')
        m = t.boot(two_partition_disk(t, first, second))
        t.expect_eq(booted_cmdline(t, m), 'partition 1', 'without one in its own volume, the first partition\'s')


@check(ports=ALL_PORTS, tags=['config'])
def config_from_smbios(t):
    """An SMBIOS type 11 string starting limine:config: is the config, ahead
    of any file. Its boot() is undefined on BIOS, so the kernel is named by
    drive there."""
    path = 'hdd(1:1):/boot/kernel.elf' if not t.port.is_uefi else 'boot():/boot/kernel.elf'
    smbios_file = t.path('smbios.txt')
    with open(smbios_file, 'w') as f:
        f.write('limine:config:' + kernel_config('from SMBIOS', path=path))
    tree = with_kernel(t)
    tree.add('/boot/limine.conf', kernel_config('from the disk'))
    m = t.boot(t.boot_disk(tree), smbios=[f'type=11,path={smbios_file}'])
    t.expect_eq(booted_cmdline(t, m), 'from SMBIOS', 'config taken from SMBIOS')


@check(ports=UEFI_PORTS, tags=['config', 'hostile'])
def config_from_smbios_out_of_reach(t):
    """[intent] A 64-bit SMBIOS entry point whose structure table runs past
    the top of the address space is passed over: the config is taken from
    the 32-bit entry point's table where the firmware gives one, else from
    the disk."""
    from cases.chainload import probe_then_limine
    smbios_file = t.path('smbios.txt')
    with open(smbios_file, 'w') as f:
        f.write('limine:config:' + kernel_config('from SMBIOS'))
    m = probe_then_limine(t, 'tables smbios3:fffffffffffff000:2000', kernel_config('from the disk'),
                          {'/boot/kernel.elf': HostFile(kernel_file(t, 'full'))},
                          smbios=[f'type=11,path={smbios_file}'])
    cmdline = booted_cmdline(t, m)
    text = m.stream(t.kernel_channel).text()
    t.check('K smbios3=0' in text.splitlines(), 'the probe put its entry point in place')
    has_32 = SMBIOS_GUID in text.lower()
    t.expect_eq(cmdline, 'from SMBIOS' if has_32 else 'from the disk',
                f'config with{"" if has_32 else "out"} a 32-bit entry point')


@check(ports=ALL_PORTS, tags=['config'])
def config_syntax(t):
    """Option names are case insensitive; comments are lines of their own; a
    value runs to the end of its line, with no quoting, spaces and all."""
    config = ('# a comment before anything\n'
              'TIMEOUT: 0\n'
              '/Test\n'
              '    # a comment inside the entry\n'
              '    Protocol: limine\n'
              '    PATH: boot():/boot/kernel.elf\n'
              '    CmdLine: "quotes" stay, as does # this\n')
    cmdline, _ = boot_config(t, config)
    t.expect_eq(cmdline, '"quotes" stay, as does # this', 'command line')


@check(ports=ALL_PORTS, tags=['config'])
def config_aliases(t):
    """kernel_path and kernel_cmdline stand for path and cmdline."""
    config = 'timeout: 0\n/Test\n    protocol: limine\n    kernel_path: boot():/boot/kernel.elf\n' \
             '    kernel_cmdline: through the aliases\n'
    cmdline, _ = boot_config(t, config)
    t.expect_eq(cmdline, 'through the aliases', 'command line')


@check(ports=ALL_PORTS, tags=['config'])
def config_value_truncation(t):
    """An option's value is truncated at 4095 characters."""
    long = ''.join(chr(ord('a') + i % 26) for i in range(5000))
    cmdline, _ = boot_config(t, kernel_config(long))
    t.expect_eq(cmdline, long[:4095], 'command line truncated to 4095 characters')


@check(ports=ALL_PORTS, tags=['config'])
def config_macros(t):
    """User macros, undefined ones expanding to nothing, and the built-in
    ARCH, FW_TYPE and LOADER_ARCH."""
    config = ('${GREETING}=hello macro\n'
              + kernel_config('${GREETING}|${UNDEFINED}|${ARCH}|${FW_TYPE}|${LOADER_ARCH}'))
    cmdline, _ = boot_config(t, config)
    fw = 'UEFI' if t.port.is_uefi else 'BIOS'
    t.expect_eq(cmdline, f'hello macro||{ARCH[t.port.name]}|{fw}|{LOADER_ARCH[t.port.name]}', 'expanded')


@check(ports=ALL_PORTS, tags=['config', 'hostile'])
def config_malformed(t):
    """[intent] Configs Limine refuses: an entry two levels below the one
    before it, or below none, directories more than 64 deep, a macro
    reference with no closing brace, a macro name or value past its buffer,
    and an enrolled config hash that is not hexadecimal. A macro may stand
    for a key."""
    leaf = '    protocol: limine\n    path: boot():/boot/kernel.elf\n'
    deep = ''.join('/' * d + f'D{d}\n' for d in range(1, 67)) + '/' * 67 + 'Leaf\n' + leaf
    cases = [
        ('timeout: 0\n/A\n///B\n' + leaf, 'config: Malformed config file. Parentless child.'),
        ('timeout: 0\n//Orphan\n' + leaf + '/A\n' + leaf, 'config: Malformed config file. Parentless child.'),
        ('timeout: 0\n' + deep, 'config: Menu nesting too deep (max 64)'),
        ('${UNCLOSED\n' + kernel_config('x'), 'config: Malformed macro usage'),
        (kernel_config('${OPEN'), 'config: Malformed macro usage'),
        ('${' + 'N' * 1024 + '}=x\n' + kernel_config('x'), 'config: Macro name too long (max 1023)'),
        ('${V}=' + 'v' * 2048 + '\n' + kernel_config('x'), 'config: Macro value too long (max 2047)'),
    ]
    for config, want in cases:
        tree = with_kernel(t)
        tree.add('/boot/limine.conf', config)
        m = t.boot(t.boot_disk(tree))
        msg, _ = wait_panic(t, m)
        t.expect_eq(msg, want, config[:24])
        m.stop()
    cmdline, m = boot_config(t, '${KEY}=timeout\n${KEY}: 0\n' + entry('Test', path='boot():/boot/kernel.elf',
                                                                       cmdline='keyed'))
    t.expect_eq(cmdline, 'keyed', 'a macro standing for the timeout key')
    m.stop()
    name = t.port.efi_name if t.port.is_uefi else 'limine-bios.sys'
    with open(t.env.bin(name), 'rb') as f:
        image = f.read()
    at = image.index(b'++CONFIG_B2SUM_SIGNATURE++') + len(b'++CONFIG_B2SUM_SIGNATURE++')
    patched = t.path(name)
    with open(patched, 'wb') as f:
        f.write(image[:at] + b'g' * 128 + image[at + 128:])
    tree = with_kernel(t)
    tree.add('/boot/limine.conf', kernel_config('x'))
    tree.add(('/EFI/BOOT/' + name) if t.port.is_uefi else '/boot/limine-bios.sys', HostFile(patched))
    msg, _ = wait_panic(t, t.boot(t.boot_disk(tree)))
    t.expect_eq(msg, '!!! INVALID CHARACTER IN CONFIG CHECKSUM !!!', 'an enrolled hash with a non-hex digit')


def many_entries_config(snapshots, last, globals_='timeout: no\n'):
    """A config as snapshot tools write it: a first entry, a directory of
    snapshots, each with a comment of its own, and last after them."""
    lines = [globals_ + '/First', '    protocol: limine', '    path: boot():/boot/kernel.elf', '    cmdline: first',
             '/Snapshots']
    for i in range(snapshots):
        lines += [f'//Snapshot {i}', f'    comment: comment of snapshot {i}', '    protocol: linux',
                  f'    path: boot():/snapshots/{i}/vmlinuz', f'    cmdline: root=snapshot{i} rw quiet']
    return '\n'.join(lines) + '\n' + last


@check(ports=ALL_PORTS, tags=['config', 'menu'])
def config_many_entries(t):
    """[intent] A config of thousands of entries, as snapshot tools write, is
    read without delay: the menu comes up at once, the last entry of a
    directory and the last entry of all keep their own comments, and the
    last one boots."""
    snapshots = 5000
    last = '/Last of many\n    comment: the last comment\n    protocol: limine\n' \
           '    path: boot():/boot/kernel.elf\n    cmdline: last of many\n'
    tree = with_kernel(t)
    tree.add('/boot/limine.conf', many_entries_config(snapshots, last))
    m = t.boot(t.boot_disk(tree))
    wait_menu(t, m)
    m.press('end')
    t.wait_text(m, 'the last comment')
    m.press('up', 'ret')
    t.wait_text(m, 'Snapshot 0')
    m.press('end', 'up')
    t.wait_text(m, f'comment of snapshot {snapshots - 1}')
    m.press('down')
    t.wait_text(m, 'the last comment')
    m.press('ret')
    t.expect_eq(booted_cmdline(t, m), 'last of many', 'the last entry booted')


@check(ports=ALL_PORTS, tags=['config'])
def config_line_ends(t):
    """[intent] Lines may end in CR LF, and blanks before a line's end are no
    part of its value, whichever way the line ends; a blank after an
    entry's slashes is no part of its name."""
    config = ('TIMEOUT: 0 \r\n'
              '/Test\t\r\n'
              '    protocol: limine\t \r\n'
              '    path: boot():/boot/kernel.elf  \r\n'
              '    cmdline: crlf value \t \r\n')
    cmdline, m = boot_config(t, config)
    t.expect_eq(cmdline, 'crlf value', 'command line')
    m.stop()
    kernel = '    protocol: limine\n    path: boot():/boot/kernel.elf\n    cmdline: x\n'
    tree = with_kernel(t)
    tree.add('/boot/limine.conf', 'timeout: no\n/\tTabbed\n' + kernel + '/ Spaced\n' + kernel + '/Plain\n' + kernel)
    scr = wait_menu(t, t.boot(t.boot_disk(tree)))
    columns = {name: scr.find_one(name)[0] for name in ('Tabbed', 'Spaced', 'Plain')}
    t.expect_eq(len(set(columns.values())), 1, f'names in one column: {columns}')


@check(ports=ALL_PORTS, tags=['config'])
def config_entry_conditions(t):
    """if_fw_type, if_arch and if_loader_arch hide entries that do not match,
    and chainload entries for the other firmware are hidden; the first entry
    left is the default."""
    fw = 'UEFI' if t.port.is_uefi else 'BIOS'
    other_fw = 'BIOS' if t.port.is_uefi else 'UEFI'
    others = [a for a in ('x86-64', 'ia-32', 'aarch64', 'riscv64', 'loongarch64') if a != ARCH[t.port.name]]
    other_loader = [a for a in ('x86-64', 'ia-32', 'aarch64', 'riscv64', 'loongarch64')
                    if a != LOADER_ARCH[t.port.name]]
    chain = entry('Other firmware chainload', protocol='bios' if t.port.is_uefi else 'efi',
                  path='boot():/nothing.efi')
    config = ('timeout: 0\n'
              + chain
              + entry('Wrong firmware', path='boot():/boot/kernel.elf', cmdline='wrong 1',
                      extra=f'if_fw_type: {other_fw}')
              + entry('Wrong architecture', path='boot():/boot/kernel.elf', cmdline='wrong 2',
                      extra=f'if_arch: {" ".join(others)}')
              + entry('Wrong loader', path='boot():/boot/kernel.elf', cmdline='wrong 3',
                      extra=f'if_loader_arch: {" ".join(other_loader)}')
              + entry('Right', path='boot():/boot/kernel.elf', cmdline='right',
                      extra=f'if_fw_type: {fw}\nif_arch: aarch64 {ARCH[t.port.name]}\n'
                            f'if_loader_arch: {LOADER_ARCH[t.port.name]} riscv64'))
    cmdline, _ = boot_config(t, config)
    t.expect_eq(cmdline, 'right', 'the one entry left shown')


def tree_config(default):
    """Entries in a tree; each boots the kernel with its own name as cmdline."""
    def e(level, title, cmdline, extra=''):
        lines = ['/' * level + title, '    protocol: limine', '    path: boot():/boot/kernel.elf',
                 f'    cmdline: {cmdline}']
        lines += ['    ' + line for line in extra.split('\n') if line]
        return '\n'.join(lines) + '\n'
    return (f'timeout: 0\ndefault_entry: {default}\n'
            + e(1, 'First', 'first')
            + '/Directory\n'
            + e(2, 'Inside', 'directory/inside')
            + e(2, 'A/B', 'slash name')
            + e(1, 'Same', 'same 0')
            + e(1, 'Same', 'same 1')
            + e(1, 'Back#slash\\', 'escapes'))


@check(ports=ALL_PORTS, tags=['config'])
def config_default_entry(t):
    """default_entry by index and by path, with \\/, \\\\ and \\# escapes and
    #N picking the Nth of same-named siblings."""
    cases = [
        ('1', 'first'),
        ('3', 'same 0'),
        ('Directory/Inside', 'directory/inside'),
        ('Directory/A\\/B', 'slash name'),
        ('Same', 'same 0'),
        ('Same#1', 'same 1'),
        ('Back\\#slash\\\\', 'escapes'),
    ]
    for default, want in cases:
        cmdline, m = boot_config(t, tree_config(default))
        t.expect_eq(cmdline, want, f'default_entry: {default}')
        m.stop()


def wait_menu(t, m, timeout=30):
    """Wait for the boot menu, keyboard help and all, on the screen."""
    return t.wait_text(m, 'ARROWS', timeout)


@check(ports=ALL_PORTS, tags=['config'])
def config_default_entry_unbootable(t):
    """A default_entry naming nothing bootable, an index or path matching
    nothing or a directory, disables autoboot even with timeout 0."""
    for default in ('0', '9', 'Nope', 'Directory/Nope', 'Directory'):
        tree = with_kernel(t)
        tree.add('/boot/limine.conf', tree_config(default))
        m = t.boot(t.boot_disk(tree))
        scr = wait_menu(t, m)
        t.check(scr.find('First'), f'default_entry: {default}: menu shown')
        t.check('B LIMINE-TEST' not in m.stream(t.kernel_channel).text(), f'default_entry: {default}: nothing booted')
        m.stop()
