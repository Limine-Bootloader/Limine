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
def authenticated_efi_entry(t):
    config = 'timeout: 0\n' + entry('EFI', protocol='efi', path='boot():/kernel.efi', cmdline='name:efi info')
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)})
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'efi', 'generic signed EFI entry started')
    t.expect_eq(report['options'], 'name:efi info', 'EFI entry receives its command line')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_without_tpm(t):
    m = authenticated_boot(t, linux_config('name:notpm info'),
                           {'/kernel.efi': signed_probe(t)}, tpm=False)
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'notpm', 'signed EFI image runs without a TPM')
    t.check('Secure Boot is active without TPM 2.0 measurements.' in
            ESCAPES.sub('', m.stream('serial').text()), 'unmeasured state is reported')


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
def authenticated_loaded_image_device_and_path(t):
    if t.port.efi_name.upper() == 'BOOTLOONGARCH64.EFI':
        # This firmware hangs with the fakeimage fixture that overwrites UEFI
        # BootServices callbacks. Still check the normal loaded-image metadata;
        # the two injected-override scenarios are not exercised on this port.
        m = authenticated_boot(t, linux_config('name:loadedimage info'),
                               {'/kernel.efi': signed_probe(t)}, tpm=False)
        report = probe_report(t, m, timeout=120)
        t.expect_eq(report['name'], 'loadedimage', 'signed EFI child started')
        t.expect_eq(report.int('file_path_is_null'), 0, 'child file path is present')
        t.expect_eq(report.int('device_handle_is_null'), 0, 'child device handle is present')
        m.stop()
        return

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
def authenticated_rejects_native_protocols(t):
    for protocol in ('limine', 'multiboot1', 'multiboot2'):
        config = 'timeout: 0\n' + entry('Forbidden', protocol=protocol, path='boot():/missing')
        m = authenticated_boot(t, config)
        message, _ = wait_panic(t, m)
        t.expect_eq(message, 'boot: Protocol is unavailable with firmware Secure Boot', protocol)
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_rejects_linux_overrides(t):
    overrides = [('dtb_path: boot():/missing.dtb', '',
                  'linux: External device trees are unavailable in authenticated boot')]
    overrides += [('', f'name:forbidden {option}',
                   'linux: Unsupported command line option in authenticated boot')
                  for option in ('initrd=/other', 'noinitrd', 'dtb=/other',
                                 '"initrd=/other"', '"noinitrd"', 'foo=initrd=/other', 'xinitrd=/other',
                                 'foo=dtb=/other', 'xdtb=/other')]
    for extra, cmdline, expected in overrides:
        m = authenticated_boot(t, linux_config(cmdline, extra=extra), {'/kernel.efi': signed_probe(t)})
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
               'root=UUID=example rw lockdown=integrity lsm=landlock,lockdown '
               'efi=nochunk iommu=on')
    m = authenticated_boot(t, linux_config(command), {'/kernel.efi': signed_probe(t)}, tpm=False)
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['options'], command, 'ordinary arguments and non-override substrings pass unchanged')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_rejects_initrdmem(t):
    config = linux_config('name:forbidden initrdmem=0x100000,4096')
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)})
    message, _ = wait_panic(t, m)
    t.expect_eq(message, 'linux: Unsupported command line option in authenticated boot', 'initrdmem')
    t.check('K name=forbidden' not in m.stream(t.kernel_channel).text(), 'child was not entered')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_rejects_inherited_initrd(t):
    m = authenticated_boot(t, linux_config(), {'/kernel.efi': signed_probe(t)}, commands='fakeinitrd')
    message, _ = wait_panic(t, m)
    t.expect_eq(message, 'linux: An initrd provider is already installed', 'inherited initrd refused')
    t.check('K fakeinitrd=0' in m.stream(t.kernel_channel).text(), 'fixture installed successfully')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_tpm_capabilities(t):
    for flags in (0x210, 0x220, 0x240):
        m = authenticated_boot(t, linux_config('name:capability info'),
                               {'/kernel.efi': signed_probe(t)}, tpm=False,
                               commands=f'faketcg2:{flags:x}')
        report = probe_report(t, m, timeout=120)
        t.expect_eq(report['name'], 'capability', f'optional TPM capability {flags:x}')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_tpm_sha3_banks(t):
    for flags in (0x1200, 0x101200):
        m = authenticated_boot(t, linux_config('name:sha3 info'), {'/kernel.efi': signed_probe(t)},
                               tpm=False, commands=f'faketcg2:{flags:x}')
        report = probe_report(t, m, timeout=120)
        t.expect_eq(report['name'], 'sha3', f'SHA3 log with capabilities {flags:x} boots')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_load_options_measurement(t):
    """Measure the UTF-16 load options including their terminal NUL in PCR12."""
    command = 'name:options info'
    m = authenticated_boot(t, linux_config(command), {'/kernel.efi': signed_probe(t)},
                           tpm=False, commands='faketcg2:200')
    m.wait_for(t.kernel_channel, 'K name=options', 120)
    m.wait_for(t.kernel_channel, 'K options_last_unit=0', 30)
    data = command.encode('utf-16-le') + b'\0\0'
    measurements = tcg2_extends(m.stream(t.kernel_channel).text())
    t.check(any((pcr, kind, digest, size) == (12, 0xd, int(fnv1a(data), 16), len(data))
                for pcr, kind, digest, size, _ in measurements),
            f'PCR12 received the final LoadOptions: {measurements}')


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


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_options_and_config_measurement(t):
    for command in ('', 'name:unicode info text:\u017c\U0001f642'):
        config = 'measured_boot: no\n' + linux_config(command)
        m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)},
                               tpm=False, commands='faketcg2:200 info')
        if command:
            m.wait_for(t.kernel_channel, 'K name=unicode', 120)
        else:
            start = m.wait_for(t.kernel_channel, 'B CHAIN-RETURN-PENDING', 120)
            m.wait_for(t.kernel_channel, 'K options_is_null=1', 120, start=start)
        measurements = tcg2_extends(m.stream(t.kernel_channel).text())
        if command:
            data = command.encode('utf-16-le') + b'\0\0'
            t.check(any((pcr, kind, digest, size) == (12, 0xd, int(fnv1a(data), 16), len(data))
                        for pcr, kind, digest, size, _ in measurements),
                    f'PCR12 contains exact UTF-16 options: {measurements}')
        else:
            t.check(not any(pcr == 12 for pcr, _, _, _, _ in measurements),
                    f'empty options do not extend PCR12: {measurements}')
        data = config.encode()
        t.check(any((pcr, kind, digest, size) == (5, 0x6, int(fnv1a(data), 16), len(data))
                    for pcr, kind, digest, size, _ in measurements),
                f'measured_boot: no cannot disable PCR5 config measurement: {measurements}')
        m.stop()


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_tpm_volume_full(t):
    """EFI_VOLUME_FULL during HashLogExtendEvent warns once and boots since PCRs were extended."""
    m = authenticated_boot(t, linux_config('name:volfull info'), {'/kernel.efi': signed_probe(t)},
                           tpm=False, commands='faketcg2:200200')
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'volfull', 'boot proceeds after EFI_VOLUME_FULL')
    serial = ESCAPES.sub('', m.stream('serial').text())
    t.expect_eq(serial.count('WARNING: tpm: The event log is full'), 1, 'log full warning shown once')
    t.check(CC_PROMPT not in serial, 'no prompt on EFI_VOLUME_FULL')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_tpm_unknown_algorithm(t):
    """Unknown digest algorithms in Spec ID Event03 are tolerated when required banks are present."""
    m = authenticated_boot(t, linux_config('name:unknownalg info'), {'/kernel.efi': signed_probe(t)},
                           tpm=False, commands='faketcg2:400200')
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'unknownalg', 'unknown Spec ID algorithm tolerated')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_cc_measurements(t):
    """Confidential computing platforms measure config and LoadOptions without interactive bypass."""
    from cases.tpm import cc_extends
    command = 'name:ccauth info'
    config = 'measured_boot: no\n' + linux_config(command)
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)},
                           tpm=False, commands='fakecc:0')
    m.wait_for(t.kernel_channel, 'K name=ccauth', 120)
    text = m.stream(t.kernel_channel).text()
    evs = cc_extends(text)
    cfg_bytes = config.encode()
    opt_bytes = command.encode('utf-16-le') + b'\0\0'
    t.check(any((pcr, mr, kind, digest, size) == (5, 2, 0x6, int(fnv1a(cfg_bytes), 16), len(cfg_bytes))
                for pcr, mr, kind, digest, size, _ in evs),
            f'CC measured config into PCR5/MR2: {evs}')
    t.check(any((pcr, mr, kind, digest, size) == (12, 3, 0xd, int(fnv1a(opt_bytes), 16), len(opt_bytes))
                for pcr, mr, kind, digest, size, _ in evs),
            f'CC measured LoadOptions into PCR12/MR3: {evs}')
    m.stop()

    m = authenticated_boot(t, linux_config('name:ccfail info'), {'/kernel.efi': signed_probe(t)},
                           tpm=False, commands='fakecc:2')
    message, screen = wait_panic(t, m)
    t.check(message.startswith('tpm: authenticated boot measurement failed'),
            f'CC measurement failure panics: {message}')
    t.check(CC_PROMPT not in screen_text(screen), 'no prompt permits unmeasured CC boot')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_efi_entry_ignores_global_dtb(t):
    """global_dtb in config does not block generic protocol: efi entries under Secure Boot."""
    config = ('timeout: 0\nglobal_dtb: boot():/missing.dtb\n'
              + entry('EFI', protocol='efi', path='boot():/kernel.efi', cmdline='name:efidtb info'))
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)}, tpm=False)
    report = probe_report(t, m, timeout=120)
    t.expect_eq(report['name'], 'efidtb', 'protocol: efi ignores global_dtb')


@check(ports=UEFI_PORTS, tags=TAGS)
def authenticated_missing_initrd_error(t):
    """Missing initrd files report a clear open error rather than a filesystem error."""
    config = linux_config(modules=[('boot():/missing.img', None)])
    m = authenticated_boot(t, config, {'/kernel.efi': signed_probe(t)}, tpm=False)
    message, _ = wait_panic(t, m)
    t.expect_eq(message, 'linux: Failed to open initrd with path `boot():/missing.img`',
                'missing initrd error message')


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
