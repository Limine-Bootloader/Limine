import hashlib

from harness.api import check
from harness.media import HostFile, Tree
from harness.report import fnv1a, unescape
from harness.ports import UEFI_PORTS
from harness import authenticode, efivars, secureboot
from cases._common import entry, kernel_file, read, screen_text, wait_panic
from cases.chainload import probe, probe_report
from cases.secureboot import b2, limine_efi
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
                       limine_image=None, part_guid=None, app_dir='limine'):
    code, template = tpm_firmware(t, tpm)
    image = limine_image or limine_efi(t, config if enrolled else None)
    tree = Tree()
    if commands is None:
        tree.add('/EFI/BOOT/' + t.port.efi_name, HostFile(image))
        tree.add('/boot/limine.conf', config)
    else:
        tree.add('/EFI/BOOT/' + t.port.efi_name, signed_probe(t))
        tree.add('/efiprobe.cmd', f'{commands} chain:\\EFI\\{app_dir}\\{t.port.efi_name}\n')
        tree.add(f'/EFI/{app_dir}/{t.port.efi_name}', HostFile(image))
        tree.add(f'/EFI/{app_dir}/limine.conf', config)
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
def authenticated_enrolled_config(t):
    config = 'timeout: 0\n' + entry('EFI', protocol='efi', path='boot():/kernel.efi',
                                    cmdline='name:enrolled info')
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)}, enrolled=True, tpm=False)
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'enrolled', 'enrolled config verified and booted cleanly')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_optional_resource_unauthenticated_config(t):
    from harness import images
    from harness.screen import Font, builtin_font
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
def authenticated_efi_entry(t):
    config = 'timeout: 0\n' + entry('EFI', protocol='efi', path='boot():/kernel.efi', cmdline='name:efi info')
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)})
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'efi', 'generic signed EFI entry started')
    t.expect_eq(report['options'], 'name:efi info', 'EFI entry receives its command line')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_edit_config_without_resigning(t):
    limine = limine_efi(t)
    child = signed_probe(t)
    for name in ('first', 'second'):
        config = ('timeout: 0\nserial: yes\n' + entry('Linux', protocol='linux',
                  path=f'boot():/{name}.efi', cmdline=f'name:{name} info root=UUID={name} rw'))
        m = authenticated_boot(t, config, {f'/{name}.efi': child}, limine_image=limine)
        report = probe_report(t, m, timeout=120)
        t.expect_eq(report['name'], name, 'changed image selection started')
        t.expect_eq(report['options'], f'name:{name} info root=UUID={name} rw', 'changed options delivered')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_empty_load_options(t):
    """Missing and empty command lines pass a NULL pointer and zero length."""
    for have_tpm in (False, True):
        for protocol in ('linux', 'efi'):
            for cmdline in (None, ''):
                config = 'timeout: 0\nserial: yes\n' + entry('Empty', protocol=protocol,
                    path='boot():/kernel.efi', cmdline=cmdline)
                m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t),
                                       '/efiprobe.cmd': 'name:empty info\n'}, tpm=have_tpm)
                report = probe_report(t, m, timeout=120)
                label = f'{protocol} {cmdline!r} TPM={have_tpm}'
                t.expect_eq(report['name'], 'empty', f'{label} child entered')
                t.expect_eq(report.int('options_is_null'), 1, f'{label} NULL pointer')
                t.expect_eq(report.int('options_size'), 0, f'{label} zero bytes')
                m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_redundant_path_separators(t):
    data = b'initrd under a subdirectory'
    config = 'timeout: 0\n' + entry('Linux', protocol='linux', path=r'boot()://\EFI//\kernel.efi//',
                                    cmdline='name:paths info readinitrd',
                                    modules=[('boot():/dir//initrd//', None)])
    m = authenticated_boot(t, config, {'/EFI/kernel.efi': signed_probe(t), '/dir/initrd': data}, tpm=False)
    report = probe_report(t, m, timeout=120)
    t.expect_eq(unescape(report['file_path']), r'\EFI\kernel.efi', 'canonical loaded image path')
    t.expect_eq(report['initrd.hash'], fnv1a(data), 'initrd read through the EFI filesystem')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_linux_initrd(t):
    """A signed EFI Linux entry receives precisely the concatenated initrds via LoadFile2."""
    first, second = b'abc', b'last!'
    config = linux_config('name:authenticated info readinitrd',
                          [('boot():/first', None), ('boot():/second', None)])
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t), '/first': first, '/second': second})
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'authenticated', 'signed EFI image entered without configuration pinning')
    t.expect_eq(report['options'], 'name:authenticated info readinitrd', 'final command line delivered')
    t.expect_eq(report.int('options_size'), (len('name:authenticated info readinitrd') + 1) * 2,
                'LoadOptions includes its UTF-16 terminator')
    t.expect_eq(report['options_last_unit'], '0', 'LoadOptions is terminated')
    combined = first + b'\0' + second
    t.expect_eq(report.int('initrd.size'), len(combined), 'padding between archives, none after the last')
    expect_efi_error(t, report['initrd.query'], 5, 'NULL buffer requests the required size')
    expect_efi_error(t, report['initrd.small'], 5, 'short buffer is rejected')
    t.expect_eq(report.int('initrd.small_size'), len(combined), 'short query reports required size')
    t.expect_eq(report['initrd.read'], '0', 'full buffer succeeds')
    t.expect_eq(report.int('initrd.read_size'), len(combined), 'full read returns exact size')
    t.expect_eq(report['initrd.hash'], fnv1a(combined), 'all bytes, including inter-archive padding')
    expect_efi_error(t, report['initrd.boot_policy'], 3, 'BootPolicy true is unsupported')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_enrolled_linux_initrd(t):
    data = b'enrolled initrd'
    path = 'boot():/initrd'
    files = {'/kernel.efi': signed_probe(t), '/initrd': data}
    for suffix, error in ((f'#{b2(data)}', None),
                          ('', f'Secure Boot is active and URI `{path}` has no associated hash!'),
                          (f'#{b2(b"other")}', f'BLAKE2b hash for URI `{path}` does not match!')):
        config = linux_config('name:enrolled readinitrd', [(path + suffix, None)])
        m = authenticated_boot(t, config, files, enrolled=True, tpm=False)
        if error is None:
            report = probe_report(t, m, timeout=120)
            t.expect_eq(report['name'], 'enrolled', 'signed EFI image booted')
            t.expect_eq(report['initrd.hash'], fnv1a(data), 'verified initrd delivered by LoadFile2')
        else:
            message, _ = wait_panic(t, m)
            t.expect_eq(message, error, 'initrd hash enforced')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_unsigned_linux(t):
    for payload in (probe(t), b'not a PE image'):
        m = authenticated_boot(t, linux_config(), {'/kernel.efi': payload})
        message, _ = wait_panic(t, m)
        t.check('LoadImage' in message, f'firmware refused unauthenticated image: {message}')
        t.check('K name=authenticated' not in m.stream(t.kernel_channel).text(), 'child was not entered')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS, timeout=240)
def authenticated_large_initrd(t):
    """Initrd staging survives beyond the loader's early 64 MiB firmware allocation reserve."""
    data = b'i' * (70 * 1024 * 1024 - 3) + b'end'
    config = linux_config('name:large readinitrd', [('boot():/initrd', None)])
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t), '/initrd': data}, size_mb=160)
    report = probe_report(t, m, timeout=180)
    t.expect_eq(report.int('initrd.size'), len(data), 'initrd larger than 64 MiB is available')
    t.expect_eq(report['initrd.read'], '0', 'full initrd copied by LoadFile2')
    t.expect_eq(report['initrd.hash'], fnv1a(data), 'all large initrd bytes survived memory release')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_tampered_signature(t):
    image = bytearray(read(signed_probe(t).path))
    image[0x40] ^= 1
    m = authenticated_boot(t, linux_config(), {'/kernel.efi': bytes(image)})
    message, _ = wait_panic(t, m)
    t.check('LoadImage' in message, f'tampered signed image refused: {message}')
    t.check('K name=authenticated' not in m.stream(t.kernel_channel).text(),
            'tampered child was not entered')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_revoked_linux(t):
    """A valid db signature does not override an image hash revoked by dbx."""
    image = read(probe(t).path)
    image += b'\0' * (-len(image) % 8)
    digest, _ = authenticode.image_digest(image)
    m = authenticated_boot(t, linux_config(), {'/kernel.efi': signed_probe(t)}, revoked=digest)
    message, _ = wait_panic(t, m)
    t.check('LoadImage' in message, f'dbx-revoked signed image refused: {message}')
    t.check('K name=authenticated' not in m.stream(t.kernel_channel).text(), 'revoked child was not entered')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_wrong_signer(t):
    """PK authorises database updates, not EFI images in place of db."""
    key, certificate = secureboot.keys(t.env)['PK']
    image = authenticode.sign(probe(t).path, t.path('wrong-signer.efi'), key, certificate)
    m = authenticated_boot(t, linux_config(), {'/kernel.efi': HostFile(image)})
    message, _ = wait_panic(t, m)
    t.check('LoadImage' in message, f'image signed by a key absent from db refused: {message}')
    t.check('K name=authenticated' not in m.stream(t.kernel_channel).text(), 'untrusted child was not entered')



@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_returned_linux(t):
    config = linux_config('name:returning readinitrd exit:1234', [('boot():/initrd', None)])
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t), '/initrd': b'returned initrd'})
    message, screen = wait_panic(t, m)
    t.expect_eq(message, 'efi: Image returned in authenticated boot (0x1234)',
                'provider and image cleanup finished before halting on returned child')
    t.check('K initrd.read=0' in m.stream(t.kernel_channel).text(), 'child consumed the installed initrd')
    t.check('Press a key' not in screen_text(screen), 'no retry with stale boot state')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_loaded_image_metadata(t):
    m = authenticated_boot(t, linux_config('name:loadedimage info'),
                           {'/kernel.efi': signed_probe(t)}, tpm=False)
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'loadedimage', 'signed EFI child started')
    t.expect_eq(report.int('file_path_is_null'), 0, 'child file path is present')
    t.expect_eq(report.int('device_handle_is_null'), 0, 'child device handle is present')


# LoongArch64 firmware hangs with BootServices callback hooks.
@check(ports=[p for p in UEFI_PORTS if p != 'uefi-loongarch64'], tags=TAGS)
def authenticated_loaded_image_device_and_path(t):
    for mode in (1, 2):
        config = linux_config('name:fakeimage info exit:1234')
        m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)},
                               tpm=False, commands=f'fakeimage:{mode:x}')
        message, _ = wait_panic(t, m)
        t.expect_eq(message, 'efi: Image returned in authenticated boot (0x1234)', 'child returned')
        text = m.stream(t.kernel_channel).text()
        if mode == 2:
            t.check('K fakeimage.fixture=1' in text, 'alternate filesystem device installed')
        t.check(f'K fakeimage.inject={mode} 1' in text, 'firmware file path fixture installed')
        t.check('K fakeimage.device=1' in text, 'device is restored only when NULL')
        t.check('K fakeimage.path=1' in text, 'firmware file path preserved')
        t.check('K fakeimage.relative_before_load=1' in text, 'relative path released before LoadImage')
        t.check(text.count('K fakeimage.relative_free=') == 1, 'relative path released exactly once')
        t.check('K fakeimage.full_alive=1' in text, 'full path remains valid at LoadImage')
        t.check(text.count('K fakeimage.full_free=') == 1, 'full path released exactly once after return')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS, timeout=300)
def authenticated_rejects_linux_overrides(t):
    child = signed_probe(t)
    limine = limine_efi(t)
    overrides = [('dtb_path: boot():/missing.dtb', '',
                  'linux: External device trees are unavailable in authenticated boot')]
    overrides += [('', f'name:forbidden {option}',
                   'linux: Unsupported command line option in authenticated boot')
                  for option in ('initrd=/other', 'noinitrd', 'noinitrd_extra', 'noinitrd123',
                                 'dtb=/other', 'initrd', 'initrdmem', 'dtb',
                                 '"initrd=/other"', '"noinitrd"',
                                 'foo=initrd=/other', 'xinitrd=/other',
                                 'foo=dtb=/other', 'xdtb=/other')]
    for extra, cmdline, expected in overrides:
        m = authenticated_boot(t, linux_config(cmdline, extra=extra), {'/kernel.efi': child},
                               limine_image=limine, tpm=False)
        message, _ = wait_panic(t, m)
        t.expect_eq(message, expected, extra or cmdline)
        t.check('K name=forbidden' not in m.stream(t.kernel_channel).text(), 'child was not entered')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_efi_options(t):
    command = 'name:efiopts info efi=nochunk,debug'
    m = authenticated_boot(t, linux_config(command), {'/kernel.efi': signed_probe(t)})
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['options'], command, 'ordinary EFI stub options pass through')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_rejects_quoted_and_long_overrides(t):
    options = ('foo="initrd"=/other', 'x"initrd"=/other', 'foo="dtb"=/other',
               'aaaa_bbbb_cccc_dddd_initrd=/other', 'x' * 512 + 'initrd=/other')
    for option in options:
        m = authenticated_boot(t, linux_config('name:forbidden exit:1234 ' + option),
                               {'/kernel.efi': signed_probe(t)}, tpm=False)
        message, _ = wait_panic(t, m)
        t.expect_eq(message, 'linux: Unsupported command line option in authenticated boot', option)
        t.check('K name=forbidden' not in m.stream(t.kernel_channel).text(), 'child was not entered')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_accepts_non_override_options(t):
    command = ('name:allowed info foo=initrd"/other" foo=dtb"/other" xnoinitrd '
               'root=UUID=example\trw lockdown=integrity lsm=landlock,lockdown '
               'efi=nochunk iommu=on')
    m = authenticated_boot(t, linux_config(command), {'/kernel.efi': signed_probe(t)}, tpm=False)
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['options'], command.replace('\t', r'\u0009'),
                'ordinary arguments and non-override substrings pass unchanged')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_linux_option_boundaries(t):
    child = signed_probe(t)
    for option in ('foo\u00a0initrdmem=0x100000,4096', 'foo\u00e0initrdmem=0x100000,4096',
                   'foo\u00a0noinitrd', 'foo\u00e0noinitrd', 'ordinary\x01text', 'ordinary\x0btext'):
        m = authenticated_boot(t, linux_config('name:boundary ' + option), {'/kernel.efi': child}, tpm=False)
        message, _ = wait_panic(t, m)
        t.expect_eq(message, 'linux: Unsupported command line option in authenticated boot', repr(option))
        t.check('K name=boundary' not in m.stream(t.kernel_channel).text(), 'child not entered')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_rejects_initrdmem(t):
    config = linux_config('name:forbidden initrdmem=0x100000,4096')
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)})
    message, _ = wait_panic(t, m)
    t.expect_eq(message, 'linux: Unsupported command line option in authenticated boot', 'initrdmem')
    t.check('K name=forbidden' not in m.stream(t.kernel_channel).text(), 'child was not entered')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_rejects_inherited_initrd(t):
    for cmd, marker, expected in (
        ('fakeinitrd', 'K fakeinitrd=0', 'linux: An initrd provider is already installed'),
        ('fakeinitrdtable', 'K fakeinitrdtable=0', 'linux: An initrd table is already installed'),
    ):
        m = authenticated_boot(t, linux_config(), {'/kernel.efi': signed_probe(t)}, commands=cmd)
        message, _ = wait_panic(t, m)
        t.expect_eq(message, expected, cmd)
        t.check(marker in m.stream(t.kernel_channel).text(), f'{cmd} fixture installed')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_empty_initrd(t):
    for modules, files in (([], {}), ([('boot():/empty', None)], {'/empty': b''})):
        config = linux_config('name:empty readinitrd', modules)
        m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t), **files})
        report = probe_report(t, m, timeout=120)
        expect_efi_error(t, report['initrd.locate'], 0xE, 'no initrd provider for empty input')
        m.stop()

    modules = [('boot():/first', None), ('boot():/empty', None), ('boot():/last', None)]
    config = linux_config('name:mixed readinitrd', modules)
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t), '/first': b'a',
                                       '/empty': b'', '/last': b'b'})
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report.int('initrd.size'), 5, 'empty archive adds no padding')
    t.expect_eq(report['initrd.hash'], fnv1a(b'a\0\0\0b'), 'nonempty archives remain aligned')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_rejects_config_overrides(t):
    image = signed_probe(t)
    cases = [
        (linux_config(extra='module_string: forbidden'),
         'linux: Initrd command lines are unavailable in authenticated boot'),
        (linux_config(extra='module_cmdline: forbidden'),
         'linux: Initrd command lines are unavailable in authenticated boot'),
        ('global_dtb: boot():/missing\n' + linux_config(),
         'linux: External device trees are unavailable in authenticated boot'),
        (linux_config(modules=[('$boot():/initrd', None)]),
         'linux: Compressed initrd URIs are unavailable in authenticated boot'),
        ('timeout: 0\n' + entry('Linux', protocol='linux', path='$boot():/kernel.efi'),
         'efi: Compressed image URIs are unavailable in authenticated boot'),
        ('timeout: 0\n' + entry('EFI', protocol='efi', path='boot():/kernel.efi',
                                modules=[('boot():/initrd', None)]),
         'efi: Modules require the Linux protocol'),
    ]
    for config, expected in cases:
        m = authenticated_boot(t, config, {'/kernel.efi': image, '/initrd': b'data'})
        message, _ = wait_panic(t, m)
        t.expect_eq(message, expected, config)
        t.check('K name=authenticated' not in m.stream(t.kernel_channel).text(), 'child was not entered')
        m.stop()


@check(ports=['uefi-ia32', 'uefi-x86-64'], tags=TAGS)
def authenticated_firmware_errors(t):
    flag = 1 << (31 if t.port.name == 'uefi-ia32' else 63)
    for mode, expected in (
        (3, f'efi: LoadImage failure (0x{flag | 26:x}); UnloadImage failure (0x{flag | 15:x})'),
        (4, f'efi: HandleProtocol failure (0x{flag | 7:x}); UnloadImage failure (0x{flag | 15:x})'),
    ):
        m = authenticated_boot(t, linux_config(), {'/kernel.efi': signed_probe(t)},
                               tpm=False, commands=f'fakeimage:{mode:x}')
        message, _ = wait_panic(t, m)
        t.expect_eq(message, expected, 'firmware failure remains visible')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_uri_dispatch(t):
    # An invalid server would give the TFTP parser's error if dispatch ran.
    for image, module, expected in (
        ('tftp(not-an-ip):/child', None, 'uri: An EFI filesystem is required'),
        ('boot():/kernel.efi', 'tftp(not-an-ip):/rd', 'uri: An EFI filesystem is required'),
        ('boot():/kernel.efi#abc', None,
         'Hash must be 64 BLAKE3, 128 BLAKE2b, or 256 BLAKE3 characters long'),
    ):
        modules = [(module, None)] if module else []
        config = 'timeout: 0\n' + entry('Linux', protocol='linux', path=image, modules=modules)
        m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)}, tpm=False)
        message, screen = wait_panic(t, m)
        t.expect_eq(message, expected, module or image)
        t.check('Press a key' not in screen_text(screen), 'URI error cannot return to the menu')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_rejects_directories(t):
    child = signed_probe(t)
    for path, modules, expected in (
        ('boot():/EFI', [], 'efi: Failed to open image with path `boot():/EFI`. Is the path correct?'),
        ('boot():/kernel.efi', [('boot():/dir/', None)], 'linux: Failed to open initrd with path `boot():/dir/`'),
    ):
        config = 'timeout: 0\n' + entry('Directory', protocol='linux', path=path, modules=modules)
        m = authenticated_boot(t, config, {'/kernel.efi': child, '/dir/file': b''}, tpm=False)
        message, screen = wait_panic(t, m)
        t.expect_eq(message, expected, 'directory rejected before reading')
        t.check('Press a key' not in screen_text(screen), 'non-interactive halt')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_custom_config_directory(t):
    config = linux_config('name:custom info')
    files = {'/kernel.efi': signed_probe(t), '/boot/limine.conf': linux_config('name:fallback')}
    m = authenticated_boot(t, config, files, commands='info', app_dir='custom-dir', tpm=False)
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'custom', 'configuration beside the EFI image takes precedence')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_config_partition_search(t):
    from harness.media import Fat, Part
    code, _ = tpm_firmware(t, False)
    boot = Tree({'/EFI/BOOT/' + t.port.efi_name: HostFile(limine_efi(t))})
    variables = secureboot.enrolled_vars(t.env, t.port, t.path('vars.fd'))
    for image in ('boot():/child.efi', 'boot(2):/child.efi'):
        config = 'timeout: 0\n' + entry('EFI', protocol='efi', path=image, cmdline='name:otherpart info')
        files = {'/limine.conf': config, '/child.efi': signed_probe(t)}
        # Neither the boot ESP nor partition 2 supplies a configuration.
        parts = [Part(size_mb=4), Part(size_mb=64, fs=Fat(files=files))]
        disk = t.boot_disk(boot, size_mb=144, extra_parts=parts)
        m = t.boot(disk, code_file=code, vars_file=variables, **secureboot.machine_options(t.port))
        if image.startswith('boot()'):
            t.expect_eq(probe_report(t, m, timeout=120)['name'], 'otherpart', 'continued past non-filesystems')
        else:
            message, _ = wait_panic(t, m)
            t.expect_eq(message, 'uri: An EFI filesystem is required', 'selected image needs an EFI filesystem')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_missing_initrd_error(t):
    """Missing image or initrd files halt cleanly under Secure Boot even without a TPM."""
    config = 'timeout: 0\n' + entry('Linux', protocol='linux', path='boot():/missing.efi')
    m = authenticated_boot(t, config, tpm=False)
    message, screen = wait_panic(t, m)
    t.expect_eq(message, 'efi: Failed to open image with path `boot():/missing.efi`. Is the path correct?',
                'missing image error message')
    t.check('Press a key' not in screen_text(screen), 'missing image halts without menu recovery')
    m.stop()

    config = linux_config(modules=[('boot():/missing.img', None)])
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)}, tpm=False)
    message, screen = wait_panic(t, m)
    t.expect_eq(message, 'linux: Failed to open initrd with path `boot():/missing.img`',
                'missing initrd error message')
    t.check('Press a key' not in screen_text(screen), 'missing initrd halts without menu recovery')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_guid_and_fslabel_paths(t):
    """Non-boot() URIs build valid full device paths for LoadImage and initrd."""
    from cases._common import entry as make_entry
    initrd = b'guid initrd payload'
    config = ('timeout: 0\n'
              + make_entry('Linux', protocol='linux', path='fslabel(LIMINE):/kernel.efi',
                           cmdline='name:nonboot readinitrd',
                           modules=[('boot():/initrd', None)]))
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t), '/initrd': initrd}, tpm=False)
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'nonboot', 'fslabel() image booted')
    t.expect_eq(report['initrd.hash'], fnv1a(initrd), 'initrd delivered')
    m.stop()

    part_guid = '12345678-1234-4321-8765-abcdef012345'
    config = ('timeout: 0\n'
              + make_entry('Linux', protocol='linux', path=f'guid({part_guid}):/kernel.efi',
                           cmdline='name:guidboot info'))
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)},
                           tpm=False, part_guid=part_guid)
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'guidboot', 'guid() image booted')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_panic_halts_without_tpm(t):
    """Errors halt without returning to the menu under Secure Boot even without a TPM."""
    cases = [
        ('timeout: 0\n//Orphan\n    protocol: linux\n',
         'config: Malformed config file. Parentless child.'),
        ('timeout: 0\n/MissingProto\n    path: boot():/kernel.efi\n',
         'Boot protocol not specified for this entry'),
        ('timeout: 0\n' + entry('NoPath', protocol='linux'),
         'efi: Image path not specified'),
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
def authenticated_efi_protocol_aliases_and_restrictions(t):
    child = signed_probe(t)
    for proto in ('uefi', 'efi_chainload'):
        config = 'timeout: 0\n' + entry('Alias', protocol=proto, path='boot():/kernel.efi',
                                        cmdline=f'name:{proto} info')
        m = authenticated_boot(t, config, {'/kernel.efi': child}, tpm=False)
        report = probe_report(t, m, timeout=120)
        t.expect_eq(report['name'], proto, f'{proto} alias boots signed image')
        m.stop()

    for config, expected in (
        ('timeout: 0\n' + entry('EFI', protocol='efi', path='boot():/kernel.efi',
                                cmdline='name:forbidden initrd=/other'),
         'linux: Unsupported command line option in authenticated boot'),
        ('timeout: 0\n' + entry('EFI', protocol='efi', path='$boot():/kernel.efi'),
         'efi: Compressed image URIs are unavailable in authenticated boot'),
        ('timeout: 0\n' + entry('EFI', protocol='efi'),
         'efi: Image path not specified'),
    ):
        m = authenticated_boot(t, config, {'/kernel.efi': child}, tpm=False)
        message, _ = wait_panic(t, m)
        t.expect_eq(message, expected, expected)
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_path_dot_normalisation(t):
    child = signed_probe(t)
    config = ('timeout: 0\n'
              + entry('Linux', protocol='linux', path='boot():/EFI/./sub/../kernel.efi',
                      cmdline='name:norm info'))
    m = authenticated_boot(t, config, {'/EFI/kernel.efi': child}, tpm=False)
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'norm', 'normalised path booted')
    t.expect_eq(unescape(report['file_path']), r'\EFI\kernel.efi', 'canonical file path')
    m.stop()

    for bad_path in ('boot():/../kernel.efi', 'boot():/EFI/../../kernel.efi'):
        config = 'timeout: 0\n' + entry('Linux', protocol='linux', path=bad_path)
        m = authenticated_boot(t, config, {'/kernel.efi': child}, tpm=False)
        message, _ = wait_panic(t, m)
        t.expect_eq(message,
                    f'efi: Failed to open image with path `{bad_path}`. Is the path correct?',
                    f'rejected escaping path {bad_path}')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_config_directory_rejected(t):
    code, _ = tpm_firmware(t, False)
    tree = Tree({
        '/EFI/BOOT/' + t.port.efi_name: HostFile(limine_efi(t)),
        '/EFI/BOOT/limine.conf/dummy': b'not a file',
    })
    variables = secureboot.enrolled_vars(t.env, t.port, t.path('vars.fd'))
    m = t.boot(t.boot_disk(tree), code_file=code, vars_file=variables,
               **secureboot.machine_options(t.port))
    message, screen = wait_panic(t, m)
    t.expect_eq(message, 'config: Configuration file not found in authenticated boot',
                'directory named limine.conf is rejected')
    t.check('Press a key' not in screen_text(screen), 'halts without empty menu')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_smbios_config_and_tftp_rejection(t):
    child = signed_probe(t)
    smbios_cfg = 'timeout:_0;/SMBIOS;protocol:_efi;path:_boot():/kernel.efi;cmdline:_name:smbios_info'
    m = authenticated_boot(t, 'timeout: no\n', {'/kernel.efi': child}, tpm=False,
                           commands=f'smbioscfg:{smbios_cfg}')
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'smbios', 'SMBIOS Type 11 config boots signed EFI image')
    m.stop()

@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_el_torito_iso(t):
    from harness.media import Fat, Iso
    from harness.ports import Drive
    code, _ = tpm_firmware(t, False)
    esp_size = 8 * 1024 * 1024
    efiboot = t.path('efiboot.img')
    with open(efiboot, 'wb') as f:
        f.truncate(esp_size)
    esp_tree = Tree({
        '/EFI/BOOT/' + t.port.efi_name: HostFile(limine_efi(t)),
        '/EFI/BOOT/limine.conf': linux_config('name:iso info'),
        '/kernel.efi': signed_probe(t),
    })
    Fat(16, files=esp_tree).write_into(efiboot, 0, esp_size, t.workdir)
    # Decoy ISO9660 config proves Limine reads from the El Torito ESP's SFS.
    iso_tree = Tree({
        '/boot/efiboot.img': HostFile(efiboot),
        '/boot/limine.conf': 'timeout: 0\n',
    })
    iso_path = t.path('el-torito.iso')
    Iso(iso_tree, uefi_boot='/boot/efiboot.img', hybrid=False).build(iso_path, t.workdir)
    variables = secureboot.enrolled_vars(t.env, t.port, t.path('isovars.fd'))
    m = t.machine(drives=[Drive(iso_path, media='cdrom', bootindex=0)],
                  code_file=code, vars_file=variables, **secureboot.machine_options(t.port))
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'iso', 'signed image booted from pure El Torito ISO')
    m.stop()

    # Re-indexing disks after an optional lookup failure must not clobber the
    # El Torito ESP filesystem handle installed on boot_volume at UEFI entry.
    # Omit serial: yes so gterm_init() opens wallpaper: and triggers
    # disk_connect_all().
    initrd = b'el-torito-initrd'
    enrolled_cfg = (
        f'timeout: 0\nwallpaper: fslabel(MISSING):/bg.bmp#{b2(b"bg")}\n'
        + entry('Linux', protocol='linux', path='boot():/kernel.efi',
                cmdline='name:isoconnect info readinitrd',
                modules=((f'boot():/initrd#{b2(initrd)}', None),))
    )
    efiboot2 = t.path('efiboot-enrolled.img')
    with open(efiboot2, 'wb') as f:
        f.truncate(esp_size)
    esp2_tree = Tree({
        '/EFI/BOOT/' + t.port.efi_name: HostFile(limine_efi(t, enrolled_cfg)),
        '/EFI/BOOT/limine.conf': enrolled_cfg,
        '/kernel.efi': signed_probe(t),
        '/initrd': initrd,
    })
    Fat(16, files=esp2_tree).write_into(efiboot2, 0, esp_size, t.workdir)
    iso2_tree = Tree({
        '/boot/efiboot.img': HostFile(efiboot2),
        '/boot/limine.conf': 'timeout: 0\n',
    })
    iso2_path = t.path('el-torito-enrolled.iso')
    Iso(iso2_tree, uefi_boot='/boot/efiboot.img', hybrid=False).build(iso2_path, t.workdir)
    variables2 = secureboot.enrolled_vars(t.env, t.port, t.path('isovars-enrolled.fd'))
    m2 = t.machine(drives=[Drive(iso2_path, media='cdrom', bootindex=0)],
                   code_file=code, vars_file=variables2, **secureboot.machine_options(t.port))
    report2 = probe_report(t, m2, timeout=120)
    t.expect_eq(report2['name'], 'isoconnect',
                'pure El Torito boot survives disk_connect_all() re-indexing')
    t.expect_eq(report2['initrd.hash'], fnv1a(initrd),
                'pure El Torito initrd survives disk_connect_all()')
    m2.stop()


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
