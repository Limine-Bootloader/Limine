import hashlib
import shutil
import subprocess

from harness.api import check
from harness.media import HostFile, Tree
from harness.ports import UEFI_PORTS
from harness.report import unescape
from harness import images, secureboot
from harness.screen import Font, builtin_font
from cases._common import entry, kernel_file, screen_text, wait_panic

# UEFI Secure Boot: what USAGE.md ("Secure Boot") says Limine enforces once
# Secure Boot is active and a config hash is enrolled, and that without an
# enrolled hash it treats Secure Boot as inactive. [intent] marks what only
# Limine's code says.


def b2(data):
    return hashlib.blake2b(data).hexdigest()


def need_firmware(t):
    fw = secureboot.firmware(t.env, t.port)
    if fw is None:
        t.skip('no Secure Boot capable firmware image (ovmf-*-secboot.fd)')
    return fw


def limine_efi(t, config=None):
    """Limine signed with the db key, with config's hash enrolled first."""
    name = t.port.efi_name
    path = t.path(name)
    shutil.copyfile(t.env.bin(name), path)
    if config is not None:
        res = subprocess.run([t.env.bin('limine'), 'enroll-config', '--quiet', path, b2(config.encode())],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        t.require(res.returncode == 0, f'enroll-config: {res.stdout.decode()}')
    return secureboot.sign(t.env, path, t.path('signed-' + name))


def sb_boot(t, config, enrolled=True, files=None, keys=True, **machine):
    """Boot signed Limine with config, its hash enrolled unless not enrolled,
    on firmware with the checks' keys enrolled, or with none (Setup Mode)."""
    code, template = need_firmware(t)
    tree = Tree()
    tree.add('/EFI/BOOT/' + t.port.efi_name, HostFile(limine_efi(t, config if enrolled else None)))
    tree.add('/boot/limine.conf', config)
    kernel = open(kernel_file(t, 'full'), 'rb').read()
    tree.add('/boot/kernel.elf', kernel)
    for path, data in (files or {}).items():
        tree.add(path, data)
    vars_path = template
    if keys:
        vars_path = secureboot.enrolled_vars(t.env, t.port, t.path(f'sbvars{t.counter}.fd'))
        t.counter += 1
    return t.boot(t.boot_disk(tree), code_file=code, vars_file=vars_path,
                  **secureboot.machine_options(t.port), **machine)


def kernel_entry(t, hashed=True, extra='', modules=(), cmdline='secure boot'):
    kernel = open(kernel_file(t, 'full'), 'rb').read()
    path = 'boot():/boot/kernel.elf' + (f'#{b2(kernel)}' if hashed else '')
    return entry('Test', path=path, cmdline=cmdline, modules=modules, extra=extra)


@check(ports=UEFI_PORTS, tags=['secureboot'])
def sb_enforced(t):
    """With Secure Boot active and the config hash enrolled, a config whose
    paths all carry hashes boots."""
    m = sb_boot(t, 'timeout: 0\n' + kernel_entry(t))
    t.expect_eq(unescape(t.kernel_report(m)['cmdline']), 'secure boot', 'booted')


@check(ports=UEFI_PORTS, tags=['secureboot'])
def sb_path_needs_hash(t):
    """Every path must carry a hash."""
    m = sb_boot(t, 'timeout: 0\n' + kernel_entry(t, hashed=False))
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'Secure Boot is active and URI `boot():/boot/kernel.elf` has no associated hash!',
                '[intent] a kernel path without a hash')


@check(ports=UEFI_PORTS, tags=['secureboot'])
def sb_config_mismatch(t):
    """The config is checked against the enrolled hash on every boot."""
    code, _ = need_firmware(t)
    config = 'timeout: 0\n' + kernel_entry(t)
    tree = Tree()
    tree.add('/EFI/BOOT/' + t.port.efi_name, HostFile(limine_efi(t, config)))
    tree.add('/boot/limine.conf', config.replace('secure boot', 'tampered'))
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
    vars_path = secureboot.enrolled_vars(t.env, t.port, t.path('sbvars.fd'))
    m = t.boot(t.boot_disk(tree), code_file=code, vars_file=vars_path, **secureboot.machine_options(t.port))
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, '!!! CHECKSUM MISMATCH FOR CONFIG FILE !!!', '[intent] tampered config')


@check(ports=UEFI_PORTS, tags=['secureboot'])
def sb_hash_mismatch_always_panics(t):
    """hash_mismatch_panic is forced to yes."""
    config = 'timeout: 0\nhash_mismatch_panic: no\n' + kernel_entry(
        t, modules=[(f'boot():/boot/m.bin#{b2(b"something else")}', None)])
    m = sb_boot(t, config, files={'/boot/m.bin': b'module contents'})
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'BLAKE2b hash for URI `boot():/boot/m.bin` does not match!', 'a mismatch panics regardless')


@check(ports=UEFI_PORTS, tags=['secureboot'])
def sb_menu_restrictions(t):
    """The editor is off whatever editor_enabled says, and the UEFI Shell key
    is gone; [intent] the branding says Secure Boot."""
    shell = t.port.shell_name
    config = 'timeout: no\neditor_enabled: yes\n' + kernel_entry(t)
    m = sb_boot(t, config, files={'/' + shell: b'not a real shell, never loaded'})
    scr = t.wait_text(m, 'ARROWS')
    text = screen_text(scr)
    t.check('E Edit' not in text, 'no editor')
    t.check('UEFI Shell' not in text, 'no UEFI Shell key')
    t.check(', Secure Boot)' in text, '[intent] branding mentions Secure Boot')
    m.press('e')
    t.check('cmdline: secure boot' not in screen_text(t.wait_text(m, 'ARROWS')), 'E does nothing')


@check(ports=UEFI_PORTS, tags=['secureboot'])
def sb_unhashed_wallpaper_font_skipped(t):
    """A wallpaper or font without a hash is skipped, the defaults taking
    their place, rather than a panic."""
    font = bytes(b ^ 0xff for b in builtin_font())
    config = ('timeout: no\nwallpaper: boot():/bg.bmp\nterm_font: boot():/font.bin\n' + kernel_entry(t))
    m = sb_boot(t, config, files={'/bg.bmp': images.bmp(8, 8, [0xff0000] * 64), '/font.bin': font})
    scr = t.wait_text(m, 'ARROWS', font=Font())
    t.check('PANIC' not in screen_text(scr), 'menu shown in the built-in font')
    t.check(m.screendump().count_colour(0xff0000) == 0, 'no wallpaper drawn')


@check(ports=UEFI_PORTS, tags=['secureboot'])
def sb_efi_chainload(t):
    """EFI chainloading needs no hash: the firmware checks the image's signature
    instead, booting a signed one and refusing an unsigned one."""
    need_firmware(t)
    arch = {'uefi-ia32': 'ia32', 'uefi-x86-64': 'x86_64'}.get(t.port.name, t.port.arch)
    probe = t.env.payload('efi', arch, 'efiprobe.efi')
    signed = secureboot.sign(t.env, probe, t.path('probe-signed.efi'))
    for path, data in (('/signed.efi', signed), ('/unsigned.efi', probe)):
        config = 'timeout: 0\n' + entry('Chain', protocol='efi', path=f'boot():{path}', cmdline='name:sbprobe')
        m = sb_boot(t, config, files={path: open(data, 'rb').read()})
        if path == '/signed.efi':
            m.wait_for(t.kernel_channel, 'K name=sbprobe', 60)
        else:
            msg, _ = wait_panic(t, m)
            t.check(msg.startswith('efi: LoadImage failure'), f'unsigned image refused: {msg}')
        m.stop()


@check(ports=UEFI_PORTS, tags=['secureboot'])
def sb_not_enrolled(t):
    """With no config hash enrolled Secure Boot counts as inactive: paths need
    no hash and the editor is there."""
    config = 'timeout: no\n' + kernel_entry(t, hashed=False)
    m = sb_boot(t, config, enrolled=False)
    scr = t.wait_text(m, 'ARROWS')
    text = screen_text(scr)
    t.check('E Edit' in text, 'editor available')
    t.check(', Secure Boot)' not in text, '[intent] branding without Secure Boot')
    m.press('ret')
    t.expect_eq(unescape(t.kernel_report(m)['cmdline']), 'secure boot', 'a kernel without a hash boots')


@check(ports=UEFI_PORTS, tags=['secureboot'])
def sb_setup_mode(t):
    """Firmware in Setup Mode, with no platform key, does not count as Secure
    Boot being active: an enrolled config hash then enforces nothing more,
    and paths need no hash."""
    config = 'timeout: no\n' + kernel_entry(t, hashed=False)
    m = sb_boot(t, config, keys=False)
    text = screen_text(t.wait_text(m, 'ARROWS'))
    t.check(', Secure Boot)' not in text, '[intent] branding without Secure Boot')
    m.press('ret')
    t.expect_eq(unescape(t.kernel_report(m)['cmdline']), 'secure boot', '[spec] a kernel without a hash boots')

