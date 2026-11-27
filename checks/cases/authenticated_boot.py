from harness.api import check
from harness.media import HostFile, Tree
from harness.report import fnv1a, unescape
from harness.ports import UEFI_PORTS
from harness import authenticode, efivars, secureboot
from cases._common import entry, kernel_file, read, screen_text, wait_panic
from cases.chainload import probe, probe_report
from cases.secureboot import limine_efi
from cases.tpm import tpm_firmware


TAGS = ['authenticated-boot', 'secureboot', 'tpm']


def expect_efi_error(t, value, code, description):
    # EFI_STATUS has the width of UINTN: IA32 uses bit 31, other UEFI ports bit
    # 63. Preserve the error bit instead of comparing 64-bit strings.
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


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_unmatched_boot_device(t):
    m = authenticated_boot(t, linux_config(), tpm=False, commands='orphan')
    message, screen = wait_panic(t, m)
    t.expect_eq(message, 'uefi: Cannot match boot device handle in authenticated boot', 'no disk fallback')
    t.check('Press any key' not in screen_text(screen), 'does not ask to scan unrelated disks')


@check(ports=UEFI_PORTS, tags=TAGS, timeout=300)
def authenticated_secure_boot_variables(t):
    config = 'timeout: no\neditor_enabled: yes\n' + entry('Linux', protocol='linux', path='boot():/missing')
    # Only these fakesb cases reach the menu; the rest must panic.
    active = {
        0: False,  # Both variables absent.
        2: False,  # SecureBoot=0, SetupMode absent.
        10: False,  # SecureBoot=0, SetupMode=0.
        11: False,  # SecureBoot=0, SetupMode=1.
        12: True,  # SecureBoot=1, SetupMode=0.
        15: True,  # Non-volatile SecureBoot.
        17: False,  # Non-volatile SetupMode.
        18: True,  # SecureBoot=1, SetupMode absent.
        28: True,  # SecureBoot=1, SetupMode=0, AuditMode=0, DeployedMode=1.
        32: False,  # SecureBoot=0, SetupMode=1, AuditMode=1, DeployedMode=0.
    }
    for scenario in range(34):
        m = authenticated_boot(t, config, tpm=False, commands=f'fakesb:{scenario:x}')
        if scenario not in active:
            message, _ = wait_panic(t, m)
            t.expect_eq(message, 'uefi: Cannot determine the firmware Secure Boot state',
                        f'invalid firmware variables scenario {scenario}')
        else:
            text = screen_text(t.wait_text(m, 'ARROWS'))
            t.expect_eq('E Edit' in text, not active[scenario],
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
def authenticated_efi_entry_ignores_global_dtb(t):
    """global_dtb and dtb_path do not block generic protocol: efi entries under Secure Boot."""
    config = ('timeout: 0\nglobal_dtb: boot():/missing-global.dtb\n'
              + entry('EFI', protocol='efi', path='boot():/kernel.efi',
                      cmdline='name:efidtb info', extra='dtb_path: boot():/missing-entry.dtb'))
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)}, tpm=False)
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'efidtb', 'protocol: efi ignores global_dtb and dtb_path')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_secure_boot_disabled(t):
    config = 'timeout: 0\n' + entry('Native', path='boot():/native.elf', cmdline='native')
    m = authenticated_boot(t, config, {'/native.elf': HostFile(kernel_file(t, 'full'))},
                           secure_boot=False, tpm=False)
    t.expect_eq(unescape(t.kernel_report(m)['cmdline']), 'native',
                'native Limine protocol boots with Secure Boot disabled')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_menu_restrictions(t):
    config = ('timeout: no\neditor_enabled: yes\nglobal_dtb: boot():/missing.dtb\n'
              + entry('Linux', protocol='linux', path='boot():/missing'))
    m = authenticated_boot(t, config)
    text = screen_text(t.wait_text(m, 'ARROWS'))
    t.check('E Edit' not in text, 'configuration cannot enable the editor')
    t.check('UEFI Shell' not in text, 'shell shortcut is unavailable')
    m.press('e')
    t.check('protocol: linux' not in screen_text(t.wait_text(m, 'ARROWS')), 'editor shortcut does nothing')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_panic_halts_without_tpm(t):
    """Errors halt without returning to the menu under Secure Boot even without a TPM."""
    cases = [
        ('timeout: 0\n//Orphan\n    protocol: linux\n',
         'config: Malformed config file. Parentless child.'),
        ('timeout: 0\n/MissingProto\n    path: boot():/kernel.efi\n',
         'Boot protocol not specified for this entry'),
        ('timeout: 0\n' + entry('NoEntry', protocol='efi_boot_entry'),
         'efi_boot_entry: No entry specified'),
        ('timeout: 0\n' + entry('MissingEntry', protocol='efi_boot_entry',
                                extra='entry: NoSuchBootEntry'),
         "efi_boot_entry: Failed to find boot entry 'NoSuchBootEntry'"),
    ]
    for config, expected in cases:
        m = authenticated_boot(t, config, tpm=False)
        message, screen = wait_panic(t, m)
        text = screen_text(screen)
        t.expect_eq(message, expected, expected)
        t.check('System halted.' in text and 'Press a key' not in text,
                f'non-interactive halt for {expected}')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_config_directory_rejected(t):
    code, _ = tpm_firmware(t, False)
    tree = Tree({
        '/EFI/BOOT/' + t.port.efi_name: HostFile(limine_efi(t)),
    })
    variables = secureboot.enrolled_vars(t.env, t.port, t.path('vars.fd'))
    m = t.boot(t.boot_disk(tree), code_file=code, vars_file=variables,
               **secureboot.machine_options(t.port))
    message, screen = wait_panic(t, m)
    t.expect_eq(message, 'config: Configuration file not found in authenticated boot',
                'missing configuration is rejected')
    t.check('Press a key' not in screen_text(screen), 'halts without empty menu')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_pxe_rejected(t):
    code, _ = tpm_firmware(t, False)
    variables = secureboot.enrolled_vars(t.env, t.port, t.path('pxevars.fd'))
    tree = Tree({'/' + t.port.efi_name: HostFile(limine_efi(t))})
    m = t.boot_pxe(tree, t.port.efi_name, code_file=code, vars_file=variables,
                   **secureboot.machine_options(t.port))
    message, screen = wait_panic(t, m)
    t.expect_eq(message, 'uefi: PXE boot is unavailable in authenticated boot', 'PXE rejected')
    t.check('Press a key' not in screen_text(screen), 'halts without menu recovery')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_efi_boot_entry(t):
    import struct
    from harness.efivars import GLOBAL, load_option, utf16z
    code, _ = tpm_firmware(t, False)
    option = load_option('Probe Boot Entry', '\\probe.efi', utf16z('name:bootnext'))
    limine = load_option('Limine', '\\EFI\\BOOT\\' + t.port.efi_name)
    config = 'timeout: 0\n/Next\n    protocol: efi_boot_entry\n    entry: probe boot ENTRY\n'
    tree = Tree({
        '/EFI/BOOT/' + t.port.efi_name: HostFile(limine_efi(t)),
        '/boot/limine.conf': config,
        '/probe.efi': signed_probe(t),
    })
    enrolled = secureboot.enrolled_vars(t.env, t.port, t.path('vars-base.fd'))
    variables = efivars.store(enrolled, t.path('vars.fd'), [
        ('Boot0001', GLOBAL, limine),
        ('Boot0042', GLOBAL, option),
        ('BootOrder', GLOBAL, struct.pack('<HH', 0x1, 0x42)),
    ])
    m = t.boot(t.boot_disk(tree), code_file=code, vars_file=variables,
               no_reboot=False, **secureboot.machine_options(t.port))
    m.wait_for(t.kernel_channel, 'K name=bootnext', 120)
    m.qmp.cmd('query-status')
    t.check(any(e['event'] == 'RESET' for e in m.qmp.events),
            'signed BootNext target ran after reboot under Secure Boot')
