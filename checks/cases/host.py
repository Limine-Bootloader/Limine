import hashlib
import shutil
import subprocess

from harness.api import check
from harness.media import Disk, Fat, HostFile, Part, Tree, BIOS_BOOT_TYPE, ESP_TYPE
from harness.report import unescape
from cases._common import entry, kernel_file, limine_version, screen_text, wait_panic
from cases.fs import blake3

# The limine host tool. Expectations come from its --help text and USAGE.md;
# [intent] marks what only its code says, such as the wording of errors.


def limine(t, *args):
    res = subprocess.run([t.env.bin('limine'), *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return res.returncode, res.stdout.decode(), res.stderr.decode()


def kernel_tree(t, cmdline='host tool test'):
    tree = Tree()
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
    t.limine_files('timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline=cmdline), tree)
    return tree


def booted_cmdline(t, m):
    return unescape(t.kernel_report(m)['cmdline'])


def read(path):
    with open(path, 'rb') as f:
        return f.read()


@check(ports=['bios'], tags=['host'])
def host_bios_install_mbr(t):
    """bios-install makes an MBR disk boot, leaves its partition table and
    disk signature be, and --uninstall restores every byte it changed."""
    disk = t.boot_disk(kernel_tree(t), install=False, mbr_id=0x1234fedc)
    before = read(disk.path)
    code, out, err = limine(t, 'bios-install', disk.path, f'--uninstall-data-file={t.path("undo.bin")}')
    t.expect_eq(code, 0, f'install succeeded: {err}')
    after = read(disk.path)
    t.expect_eq(after[440:446], before[440:446], 'disk signature kept')
    t.expect_eq(after[446:510], before[446:510], 'partition table kept (partition 1 was active)')
    t.expect_eq(after[510:512], b'\x55\xaa', 'boot signature')
    m = t.boot(disk)
    t.expect_eq(booted_cmdline(t, m), 'host tool test', 'the installed disk boots')
    m.stop()
    code, out, err = limine(t, 'bios-install', '--uninstall', f'--uninstall-data-file={t.path("undo.bin")}', disk.path)
    t.expect_eq(code, 0, f'uninstall succeeded: {err}')
    t.check(read(disk.path) == before, 'the disk is as it was before the install')


@check(ports=['bios'], tags=['host'])
def host_bios_install_active_flag(t):
    """[intent] With no active partition, partition 1 is made active."""
    tree = kernel_tree(t)
    disk = Disk('mbr', 64, parts=[Part(fs=Fat(32, files=tree), type='0c')])
    disk.build(t.path('noactive.img'), t.workdir)
    code, out, err = limine(t, 'bios-install', t.path('noactive.img'))
    t.expect_eq(code, 0, f'install succeeded: {err}')
    t.check('No active partition found' in err, f'warned: {err!r}')
    t.expect_eq(read(t.path('noactive.img'))[446], 0x80, 'partition 1 active')
    m = t.boot(t.path('noactive.img'))
    t.expect_eq(booted_cmdline(t, m), 'host tool test', 'boots')


def gpt_disk(t, name, bios_boot=True):
    parts = [Part(size_mb=40, fs=Fat(32, files=kernel_tree(t)), type=ESP_TYPE)]
    if bios_boot:
        parts.append(Part(size_mb=2, type=BIOS_BOOT_TYPE))
    disk = Disk('gpt', 64, parts=parts)
    disk.build(t.path(name), t.workdir)
    return t.path(name)


@check(ports=['bios'], tags=['host'])
def host_bios_install_gpt(t):
    """On GPT the boot code goes in the BIOS boot partition, named by index
    or, without one, found by its type; nothing else will do."""
    for args in ((), ('2',)):
        path = gpt_disk(t, f'gpt{len(args)}.img')
        code, out, err = limine(t, 'bios-install', path, *args)
        t.expect_eq(code, 0, f'install {args}: {err}')
        if not args:
            t.check('Autodetected partition 2 as BIOS boot partition' in err, f'[intent] found by type: {err!r}')
        m = t.boot(path)
        t.expect_eq(booted_cmdline(t, m), 'host tool test', f'GPT install {args} boots')
        m.stop()
    path = gpt_disk(t, 'wrongpart.img')
    code, out, err = limine(t, 'bios-install', path, '1')
    t.check(code != 0 and 'is not of BIOS boot partition type' in err, f'[intent] refused partition 1: {err!r}')
    path = gpt_disk(t, 'nobiosboot.img', bios_boot=False)
    code, out, err = limine(t, 'bios-install', path)
    t.check(code != 0 and 'no BIOS boot partition specified or' in err, f'[intent] refused: {err!r}')


@check(ports=['bios'], tags=['host'])
def host_cli(t):
    """Usage, version and the --quiet and --uninstall guard rails."""
    code, out, err = limine(t, '--help')
    t.check(code == 0 and 'bios-install' in out and 'enroll-config' in out, 'help lists the commands')
    version = limine_version(t.env)
    for args in (('version',), ('--version',)):
        code, out, err = limine(t, *args)
        t.check(code == 0 and out.startswith(f'Limine {version}'), f'{args}: {out!r}')
    code, out, err = limine(t, '--print-datadir')
    t.check(code == 0 and out.strip().startswith('/'), f'datadir: {out!r}')
    code, out, err = limine(t, 'bios-install')
    t.check(code != 0 and out.startswith('usage: '), f'[intent] no arguments: usage: {out!r}')
    code, out, err = limine(t, 'bios-install', '--quiet')
    t.check(code != 0 and 'error: No device specified' in err, f'[intent] options but no device: {err!r}')
    disk = t.boot_disk(kernel_tree(t), install=False)
    code, out, err = limine(t, 'bios-install', disk.path, '1', '2')
    t.check(code != 0 and out.startswith('usage: '), f'[intent] a third operand: usage: {out!r}')
    code, out, err = limine(t, 'bios-install', '--uninstall', disk.path)
    t.check(code != 0 and 'no --uninstall-data-file' in err, f'[intent] uninstall needs its data: {err!r}')
    code, out, err = limine(t, 'bios-install', '--quiet', disk.path)
    t.expect_eq((code, out, err), (0, '', ''), '--quiet: nothing printed')


def enroll(t, efi, data, *extra):
    return limine(t, 'enroll-config', efi, hashlib.blake2b(data).hexdigest(), *extra)


@check(ports=['uefi-x86-64', 'bios'], tags=['host'])
def host_enroll_config(t):
    """enroll-config writes a config hash into Limine; the config must then
    match it, and the editor is off by default; --reset takes it out."""
    tree = kernel_tree(t)
    config = tree.files['/boot/limine.conf'].encode()
    name = t.port.efi_name if t.port.is_uefi else 'limine-bios.sys'
    target = t.path(name)
    shutil.copyfile(t.env.bin(name), target)
    code, out, err = enroll(t, target, config)
    t.expect_eq(code, 0, f'enrolled: {err}')
    dest = ('/EFI/BOOT/' + name) if t.port.is_uefi else '/boot/limine-bios.sys'
    tree.add(dest, HostFile(target))
    m = t.boot(t.boot_disk(tree))
    t.expect_eq(booted_cmdline(t, m), 'host tool test', 'a matching config boots')
    m.stop()
    tree.add('/boot/limine.conf', 'timeout: no\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x'))
    m = t.boot(t.boot_disk(tree))
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, '!!! CHECKSUM MISMATCH FOR CONFIG FILE !!!', '[intent] a config that does not match')
    m.stop()
    edited = ('timeout: no\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x')).encode()
    code, out, err = enroll(t, target, edited)
    t.expect_eq(code, 0, f'enrolled again: {err}')
    m = t.boot(t.boot_disk(tree))
    scr = t.wait_text(m, 'ARROWS')
    t.check('E Edit' not in screen_text(scr), '[spec] editor off by default with a hash enrolled (CONFIG.md)')
    m.stop()
    code, out, err = limine(t, 'enroll-config', '--reset', target)
    t.expect_eq(code, 0, f'reset: {err}')
    m = t.boot(t.boot_disk(tree))
    scr = t.wait_text(m, 'ARROWS')
    t.check('E Edit' in screen_text(scr), 'with the hash removed, any config, and the editor back')
    code, out, err = limine(t, 'enroll-config', target, 'ab' * 32)
    t.check(code != 0, f'[spec] a 64 character hash is not one enroll-config takes: {err!r}')


@check(ports=['uefi-x86-64', 'bios'], tags=['host'])
def host_enroll_config_blake3(t):
    """enroll-config takes a BLAKE3 hash with 128-byte extended output too
    (USAGE.md), which the config must then match."""
    tree = kernel_tree(t)
    config = tree.files['/boot/limine.conf'].encode()
    name = t.port.efi_name if t.port.is_uefi else 'limine-bios.sys'
    target = t.path(name)
    shutil.copyfile(t.env.bin(name), target)
    code, out, err = limine(t, 'enroll-config', target, blake3(config, 128))
    t.expect_eq(code, 0, f'enrolled: {err}')
    tree.add(('/EFI/BOOT/' + name) if t.port.is_uefi else '/boot/limine-bios.sys', HostFile(target))
    m = t.boot(t.boot_disk(tree))
    t.expect_eq(booted_cmdline(t, m), 'host tool test', '[spec] a matching config boots')
    m.stop()
    tree.add('/boot/limine.conf', 'timeout: no\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x'))
    m = t.boot(t.boot_disk(tree))
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, '!!! CHECKSUM MISMATCH FOR CONFIG FILE !!!', '[intent] a config that does not match')
