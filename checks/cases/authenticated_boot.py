from harness.api import check
from harness.media import HostFile, Tree
from harness.report import fnv1a, unescape
from harness.ports import UEFI_PORTS
from harness import authenticode, efivars, secureboot
from cases._common import entry, kernel_file, read, screen_text, wait_panic
from cases.chainload import probe, probe_report
from cases.secureboot import limine_efi
from cases.tpm import CC_PROMPT, ESCAPES, tcg2_extends, tpm_firmware


TAGS = ['authenticated-boot', 'secureboot', 'tpm']


def expect_efi_error(t, value, code, description):
    # EFI_STATUS has the width of UINTN: IA32 uses bit 31, other UEFI ports bit 63.
    # Preserve the error bit in the assertion instead of comparing 64-bit strings.
    actual = int(value, 16)
    error_bit = 31 if t.port.efi_name.upper() == 'BOOTIA32.EFI' else 63
    t.expect_eq(actual, (1 << error_bit) | code, description)


def signed_probe(t):
    return HostFile(secureboot.sign(t.env, probe(t).path, t.path('signed-probe.efi')))


def authenticated_boot(t, config, files=None, *, keys=True, tpm=True, commands=None,
                       size_mb=64, revoked=None, secure_boot=True, enrolled=False,
                       limine_image=None, part_guid=None):
    code, template = tpm_firmware(t, tpm)
    image = limine_image or limine_efi(t, config if enrolled else None)
    tree = Tree()
    if commands is None:
        tree.add('/EFI/BOOT/' + t.port.efi_name, HostFile(image))
        tree.add('/boot/limine.conf', config)
    else:
        tree.add('/EFI/BOOT/' + t.port.efi_name, signed_probe(t))
        tree.add('/efiprobe.cmd', f'{commands} chain:\\EFI\\limine\\{t.port.efi_name}\n')
        tree.add('/EFI/limine/' + t.port.efi_name, HostFile(image))
        tree.add('/EFI/limine/limine.conf', config)
    for path, data in (files or {}).items():
        tree.add(path, data)
    variables = secureboot.enrolled_vars(t.env, t.port, t.path('sbvars.fd')) if keys else template
    if not secure_boot:
        variables = efivars.write(variables, t.path('sb-disabled.fd'), {
            secureboot.SECURE_BOOT_ENABLE: (secureboot.NV_BS, efivars.NO_TIME, b'\0')})
    if revoked is not None:
        dbx = secureboot.signature_list(secureboot.CERT_SHA256, secureboot.DBX_OWNER, revoked)
        variables = efivars.write(variables, t.path('dbxvars.fd'), {
            ('dbx', secureboot.IMAGE_SECURITY): (secureboot.AUTHENTICATED, secureboot.DBX_TIME, dbx)})
    disk = t.boot_disk(tree, size_mb=size_mb, part_guid=part_guid)
    return t.boot(disk, code_file=code, vars_file=variables,
                  tpm=t.software_tpm() if tpm else None, **secureboot.machine_options(t.port))


def linux_config(cmdline='name:authenticated info', modules=(), extra=''):
    return 'timeout: 0\nserial: yes\n' + entry('Linux', protocol='linux', path='boot():/kernel.efi',
                                              cmdline=cmdline, modules=modules, extra=extra)


@check(ports=UEFI_PORTS, tags=TAGS, timeout=300)
def authenticated_secure_boot_variables(t):
    config = 'timeout: no\neditor_enabled: yes\n' + entry('Linux', protocol='linux', path='boot():/missing')
    for scenario in range(15):
        m = authenticated_boot(t, config, tpm=False, commands=f'fakesb:{scenario:x}')
        if 1 <= scenario <= 9 or scenario >= 13:
            message, _ = wait_panic(t, m)
            t.expect_eq(message, 'uefi: Cannot determine the firmware Secure Boot state',
                        f'invalid firmware variables scenario {scenario}')
        else:
            text = screen_text(t.wait_text(m, 'ARROWS'))
            t.expect_eq('E Edit' in text, scenario != 12,
                        f'editor follows runtime Secure Boot scenario {scenario}')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_setup_mode_legacy(t):
    """Setup Mode retains the native loader and interactive editor."""
    config = 'timeout: no\neditor_enabled: yes\n' + entry('Linux', protocol='linux', path='boot():/missing')
    m = authenticated_boot(t, config, keys=False)
    text = screen_text(t.wait_text(m, 'ARROWS'))
    t.check('E Edit' in text, 'Setup Mode leaves the editor available')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_secure_boot_disabled(t):
    config = 'timeout: 0\n' + entry('Native', path='boot():/native.elf', cmdline='native')
    m = authenticated_boot(t, config, {'/native.elf': HostFile(kernel_file(t, 'full'))},
                           secure_boot=False, tpm=False)
    t.expect_eq(unescape(t.kernel_report(m)['cmdline']), 'native',
                'native Limine protocol boots with Secure Boot disabled')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_menu_restrictions(t):
    config = 'timeout: no\neditor_enabled: yes\n' + entry('Linux', protocol='linux', path='boot():/missing')
    m = authenticated_boot(t, config)
    text = screen_text(t.wait_text(m, 'ARROWS'))
    t.check('E Edit' not in text, 'configuration cannot enable the editor')
    t.check('UEFI Shell' not in text, 'shell shortcut is unavailable')
    m.press('e')
    t.check('protocol: linux' not in screen_text(t.wait_text(m, 'ARROWS')), 'editor shortcut does nothing')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_optional_resource_unauthenticated_config(t):
    from harness import images
    from harness.screen import Font, builtin_font
    from cases.secureboot import b2
    bg = images.bmp(8, 8, [0xff0000] * 64)
    font = bytes(b ^ 0xff for b in builtin_font())
    config = (f'timeout: no\nwallpaper: boot():/bg.bmp#{b2(bg)}\n'
              f'term_font: boot():/font.bin#{b2(font)}\n'
              + entry('Linux', protocol='linux', path='boot():/missing'))
    m = authenticated_boot(t, config, {'/bg.bmp': bg, '/font.bin': font},
                           enrolled=False, tpm=False)
    scr = t.wait_text(m, 'ARROWS', font=Font())
    t.check('PANIC' not in screen_text(scr), 'menu shown in built-in font')
    t.check(m.screendump().count_colour(0xff0000) == 0, 'hashed wallpaper skipped without enrolled config')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_empty_options_incomplete_log(t):
    config = linux_config('')
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)},
                           tpm=False, commands='faketcg2:208')
    message, _ = wait_panic(t, m)
    t.expect_eq(message, 'tpm: authenticated boot requires a complete event log',
                'empty options cannot bypass log validation')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_tpm_invalid_sha3_logs(t):
    for flags in (0x3200, 0x21200, 0x41200, 0x5200, 0x9200, 0x11200, 0x81200):
        m = authenticated_boot(t, linux_config(), tpm=False, commands=f'faketcg2:{flags:x}')
        message, _ = wait_panic(t, m)
        t.check('event log is malformed' in message or 'missing active PCR banks' in message,
                f'invalid SHA3 log {flags:x} refused: {message}')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_tpm_measurement_failure(t):
    for flags in (0x0, 0x202, 0x208, 0x600, 0xa00):
        m = authenticated_boot(t, linux_config(), tpm=False, commands=f'faketcg2:{flags:x}')
        message, screen = wait_panic(t, m)
        t.check(message.startswith('tpm: authenticated boot measurement failed')
                or message == 'tpm: authenticated boot requires a complete event log'
                or message == 'tpm: authenticated boot event log is malformed'
                or message.startswith('tpm: Measurement'),
                f'failed or unlogged measurement refused: {message}')
        t.check(CC_PROMPT not in screen_text(screen), 'no prompt permits unmeasured boot')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_enrolled_config(t):
    config = 'timeout: 0\n' + entry('EFI', protocol='efi', path='boot():/kernel.efi',
                                    cmdline='name:enrolled info')
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)}, enrolled=True, tpm=False)
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'enrolled', 'enrolled config verified and booted cleanly')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_efi_entry(t):
    config = 'timeout: 0\n' + entry('EFI', protocol='efi', path='boot():/kernel.efi', cmdline='name:efi info')
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)})
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'efi', 'generic signed EFI entry started')
    t.expect_eq(report['options'], 'name:efi info', 'EFI entry receives its command line')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_efi_entry_ignores_global_dtb(t):
    """global_dtb in config does not block generic protocol: efi entries under Secure Boot."""
    config = ('timeout: 0\nglobal_dtb: boot():/missing.dtb\n'
              + entry('EFI', protocol='efi', path='boot():/kernel.efi', cmdline='name:efidtb info'))
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)}, tpm=False)
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'efidtb', 'protocol: efi ignores global_dtb')
