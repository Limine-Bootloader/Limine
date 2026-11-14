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
