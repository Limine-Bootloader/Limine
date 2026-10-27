import re
import struct
import time

from harness.api import check
from harness import efivars
from harness.efivars import GLOBAL, LOADER, load_option, utf16z
from harness.media import HostFile, Tree
from harness.ports import UEFI_PORTS
from cases._common import (boot_entry, check_report_clean, countdown_start, entry, kernel_file, limine_version,
                           screen_text, wait_panic)
from cases.menu import booted_cmdline, menu_entry, nothing_booted, tree_menu

# What only UEFI Limine does: the Boot Loader Interface (systemd.io's document,
# which carries no version; CONFIG.md for how it ranks against the config),
# efi_boot_entry, and the firmware setup and UEFI Shell keys. [intent] marks
# what only Limine's code says, such as how it derives entry identifiers.

PROBE = 'boot():/probe.efi'


def probe_file(t):
    arch = {'uefi-ia32': 'ia32', 'uefi-x86-64': 'x86_64'}.get(t.port.name, t.port.arch)
    return t.env.payload('efi', arch, 'efiprobe.efi')


def uefi_boot(t, config, variables=(), files=None, **machine):
    """Boot Limine with config from a disk holding the check executable and the
    probe, on a store with variables set; return (machine, disk)."""
    tree = Tree()
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
    tree.add('/probe.efi', HostFile(probe_file(t)))
    for path, data in (files or {}).items():
        tree.add(path, data)
    t.limine_files(config, tree)
    disk = t.boot_disk(tree)
    t.counter += 1
    store = efivars.store(t.env.ovmf(t.port.vars), t.path(f'vars{t.counter}.fd'), variables)
    return t.boot(disk, vars_file=store, **machine), disk


def stored(m):
    """The variables the machine's store held when it stopped."""
    m.stop()
    return efivars.read(m.workdir + '/vars.fd')


def var_data(report, name):
    value = report[f'var.{name}']
    if value == 'missing':
        return None
    return bytes.fromhex(value.split(' hex ', 1)[1])


def var_text(report, name):
    data = var_data(report, name)
    return None if data is None else data.decode('utf-16-le').rstrip('\0')


def probe_entry(level, title, commands):
    return f'{"/" * level}{title}\n    protocol: efi\n    path: {PROBE}\n    cmdline: {commands}\n'


@check(ports=UEFI_PORTS, tags=['uefi'])
def uefi_bli_loader_variables(t):
    """The variables Limine sets for the OS: the ESP it ran from, when it
    started and handed over, its entries in menu order and the one booted."""
    config = ('timeout: 0\ndefault_entry: Dir/Probe\n' + menu_entry(1, 'First', 'first') + menu_entry(1, '+Dir')
              + probe_entry(2, 'Probe', f'vendor:{LOADER}') + menu_entry(1, 'Last', 'last'))
    m, disk = uefi_boot(t, config)
    r = t.kernel_report(m, begin='B EFIPROBE')
    t.expect_eq(var_text(r, 'LoaderDevicePartUUID').lower(), disk.parts[0].guid.lower(),
                '[spec] LoaderDevicePartUUID: the ESP\'s partition GUID')
    init, exec_ = var_text(r, 'LoaderTimeInitUSec'), var_text(r, 'LoaderTimeExecUSec')
    t.check(init is not None and exec_ is not None and init.isdigit() and exec_.isdigit() and int(init) <= int(exec_),
            f'[spec] LoaderTimeInitUSec {init!r} and LoaderTimeExecUSec {exec_!r}: decimal, in that order')
    entries = var_data(r, 'LoaderEntries')
    t.expect_eq(entries and entries.decode('utf-16-le').split('\0')[:-1], ['First', 'Dir.Probe', 'Last'],
                '[spec] LoaderEntries in menu order ([intent] identifiers from the entry paths)')
    t.expect_eq(var_text(r, 'LoaderEntrySelected'), 'Dir.Probe', '[spec] LoaderEntrySelected: the entry booted')
    t.expect_eq(var_text(r, 'LoaderInfo'), f'Limine {limine_version(t.env)}', '[intent] LoaderInfo')
    features = var_data(r, 'LoaderFeatures')
    t.expect_eq(features and struct.unpack('<Q', features)[0], 0b10000000001111,
                '[intent] LoaderFeatures: timeouts, entries, one-shots and menu-disabled')


def var(name, value):
    return (name, LOADER, utf16z(value))


@check(ports=UEFI_PORTS, tags=['uefi'])
def uefi_bli_entry_requests(t):
    """LoaderEntryOneShot picks the entry for one boot, over default_entry,
    and is used up; LoaderEntryDefault picks it only where default_entry is
    not set, and stays."""
    for globals_, variables, expect in (
            ('timeout: 0\ndefault_entry: 1\n', [var('LoaderEntryOneShot', 'Dir.Inner-Two')], 'inner two'),
            ('timeout: 0\ndefault_entry: 1\n', [var('LoaderEntryOneShot', 'Collapsed.Hidden-Child')], 'hidden child'),
            ('timeout: 0\n', [var('LoaderEntryDefault', 'Last')], 'last'),
            ('timeout: 0\ndefault_entry: 1\n', [var('LoaderEntryDefault', 'Last')], 'first')):
        m, _ = uefi_boot(t, tree_menu(globals_), variables)
        name = variables[0][0]
        t.expect_eq(booted_cmdline(t, m), expect, f'[spec] {name} {variables[0][2]!r} with {globals_!r}')
        left = stored(m)
        if name == 'LoaderEntryOneShot':
            t.check((name, LOADER) not in left, f'[spec] {name} removed once read')
        else:
            t.check((name, LOADER) in left, f'[spec] {name} kept')


@check(ports=UEFI_PORTS, tags=['uefi'])
def uefi_bli_entry_ids(t):
    """[intent] An identifier is the entry's path, with what a filename may
    not hold made a dash: a second entry of the same name is Twin#1, so
    Twin-1; names that come out alike, as A B and A-B do, number the later
    one; one that comes out as . or .. gets a dash. LoaderEntryDefault may
    hold an entry path instead, as an older Limine wrote it."""
    config = ('timeout: 0\ndefault_entry: 4\n' + menu_entry(1, 'Twin', 'one') + menu_entry(1, 'Twin', 'two')
              + menu_entry(1, 'A B', 'space') + probe_entry(1, 'A-B', f'vendor:{LOADER}') + menu_entry(1, '.', 'dot')
              + menu_entry(1, '..', 'dotdot') + menu_entry(1, '+Dir') + menu_entry(2, 'Second', 'second'))
    m, _ = uefi_boot(t, config)
    r = t.kernel_report(m, begin='B EFIPROBE')
    entries = var_data(r, 'LoaderEntries')
    t.expect_eq(entries and entries.decode('utf-16-le').split('\0')[:-1],
                ['Twin', 'Twin-1', 'A-B', 'A-B-2', '.-', '..-', 'Dir.Second'], 'LoaderEntries')
    t.expect_eq(var_text(r, 'LoaderEntrySelected'), 'A-B-2', 'LoaderEntrySelected: the entry named A-B')
    m.stop()
    m, _ = uefi_boot(t, config.replace('default_entry: 4\n', ''), [var('LoaderEntryDefault', 'Dir/Second')])
    t.expect_eq(booted_cmdline(t, m), 'second', 'LoaderEntryDefault holding a path')


@check(ports=UEFI_PORTS, tags=['uefi', 'config'])
def uefi_bli_many_entries(t):
    """[intent] With more entries than LoaderEntries has room for, as many
    identifiers as fit its 2048 characters are published, whole and in menu
    order, and the boot goes on."""
    from cases.config import many_entries_config
    snapshots = 3000
    m, _ = uefi_boot(t, many_entries_config(snapshots, probe_entry(1, 'Probe', f'vendor:{LOADER}'),
                                            'timeout: 0\ndefault_entry: Probe\n'))
    r = t.kernel_report(m, begin='B EFIPROBE')
    data = var_data(r, 'LoaderEntries')
    t.require(data is not None, 'LoaderEntries set')
    ids = data.decode('utf-16-le').split('\0')[:-1]
    want = ['First'] + [f'Snapshots.Snapshot-{i}' for i in range(snapshots)] + ['Probe']
    t.expect_eq(ids, want[:len(ids)], 'whole identifiers, in menu order')
    used = sum(len(i) + 1 for i in ids)
    t.check(used <= 2048 < used + len(want[len(ids)]) + 1, f'as many as fit: {len(ids)}, {used} characters')


@check(ports=UEFI_PORTS, tags=['uefi', 'hostile'])
def uefi_exit_boot_services_retried(t):
    """[spec] ExitBootServices() refusing a stale map key is retried with the
    map fetched again (UEFI 2.11 section 7.4.6), here a map that has grown past
    the buffer kept for it; one that never succeeds stops the boot [intent]."""
    from cases.chainload import probe_then_limine
    from cases._common import wait_panic
    config = 'timeout: 0\n' + menu_entry(1, 'Test', 'retried')
    files = {'/boot/kernel.elf': HostFile(kernel_file(t, 'full'))}
    m = probe_then_limine(t, 'ebsfail:1:200', config, files)
    t.expect_eq(booted_cmdline(t, m), 'retried', 'booted after one refusal')
    m.stop()
    m = probe_then_limine(t, 'ebsfail:ffffffff', config, files)
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'efi: Failed to exit boot services', 'refused every time')


def countdown_shown(t, m):
    scr = t.wait_text(m, 'ARROWS')
    time.sleep(1.5 * t.port.timeout_scale)
    return 'Booting automatically' in screen_text(t.screen(m)), scr


@check(ports=UEFI_PORTS, tags=['uefi'])
def uefi_bli_timeout_requests(t):
    """LoaderConfigTimeoutOneShot overrides timeout for one boot, 0 meaning
    no timeout at all, and is used up; LoaderConfigTimeout applies only where
    timeout is not set: menu-force shows the menu without a timeout, 0 and
    menu-disabled boot the default entry without one."""
    m, _ = uefi_boot(t, tree_menu('timeout: 0\n'), [var('LoaderConfigTimeoutOneShot', '0')])
    shown, _ = countdown_shown(t, m)
    t.check(not shown and nothing_booted(t, m, 4), '[spec] one-shot 0: the menu with no timeout')
    t.check(('LoaderConfigTimeoutOneShot', LOADER) not in stored(m), '[spec] the one-shot removed once read')

    m, _ = uefi_boot(t, tree_menu('timeout: 0\nserial: yes\n'), [var('LoaderConfigTimeoutOneShot', '7')])
    t.expect_eq(countdown_start(t, m), '7', '[spec] one-shot 7 over timeout: 0')
    m.stop()

    m, _ = uefi_boot(t, tree_menu('serial: yes\n'), [var('LoaderConfigTimeout', '3')])
    t.expect_eq(countdown_start(t, m), '3', '[spec] LoaderConfigTimeout 3 where timeout is not set')
    m.stop()

    m, _ = uefi_boot(t, tree_menu(''), [var('LoaderConfigTimeout', 'menu-force')])
    shown, _ = countdown_shown(t, m)
    t.check(not shown and nothing_booted(t, m, 4), '[spec] menu-force: the menu with no timeout')
    m.stop()

    m, _ = uefi_boot(t, tree_menu('timeout: 0\n'), [var('LoaderConfigTimeout', 'menu-force')])
    t.expect_eq(booted_cmdline(t, m), 'first', '[spec] timeout: 0 set in the config wins over LoaderConfigTimeout')
    m.stop()

    for value in ('0', 'menu-disabled'):
        m, _ = uefi_boot(t, tree_menu(''), [var('LoaderConfigTimeout', value)])
        shown = False
        while 'E DONE' not in m.stream(t.kernel_channel).text() and not m.exited():
            shown = shown or 'ARROWS' in screen_text(t.screen(m, 'poll'))
            time.sleep(0.3)
        t.expect_eq(booted_cmdline(t, m), 'first', f'[spec] LoaderConfigTimeout {value}: the default entry booted')
        t.check(not shown, f'[spec] LoaderConfigTimeout {value}: without the menu shown')
        m.stop()


@check(ports=UEFI_PORTS, tags=['uefi'])
def uefi_efi_boot_entry(t):
    """efi_boot_entry reboots into the firmware boot option whose description
    is entry, [intent] matched regardless of case; [intent] it panics when
    there is no such option or no entry option."""
    option = load_option('Probe Boot Entry', '\\probe.efi', utf16z('name:bootnext'))
    limine = load_option('Limine', '\\EFI\\BOOT\\' + t.port.efi_name)
    config = 'timeout: 0\n/Next\n    protocol: efi_boot_entry\n    entry: probe boot ENTRY\n'
    # Limine's own option first: the probe's, which the firmware's
    # expansion ties to the same disk, would otherwise run first.
    m, _ = uefi_boot(t, config, [('Boot0001', GLOBAL, limine), ('Boot0042', GLOBAL, option),
                                 ('BootOrder', GLOBAL, struct.pack('<HH', 0x1, 0x42))], no_reboot=False)
    m.wait_for(t.kernel_channel, 'K name=bootnext', 120)
    # The events that came in meanwhile are read with the next reply.
    m.qmp.cmd('query-status')
    t.check(any(e['event'] == 'RESET' for e in m.qmp.events), 'the probe started after a reboot, not instead of Limine')
    m.stop()
    for entry_line, message in (('    entry: Nope\n', "efi_boot_entry: Failed to find boot entry 'Nope'"),
                                ('', 'efi_boot_entry: No entry specified')):
        m, _ = uefi_boot(t, 'timeout: 0\n/Next\n    protocol: efi_boot_entry\n' + entry_line)
        msg, _ = wait_panic(t, m)
        t.expect_eq(msg, message, '[intent] panic')
        m.stop()


@check(ports=UEFI_PORTS, tags=['uefi'])
def uefi_firmware_setup_key(t):
    """S reboots into the firmware's own interface [intent]: edk2, asked to,
    shows its boot manager menu or its front page."""
    m, _ = uefi_boot(t, tree_menu(), no_reboot=False)
    text = screen_text(t.wait_text(m, 'ARROWS'))
    t.check('S Firmware Setup' in text, '[intent] the key offered')
    m.press('s')
    t.wait_screen(m, lambda s: any(ui in screen_text(s) for ui in ('Please select boot device', 'Boot Manager')),
                  90, desc="the firmware's interface", firmware=True)


@check(ports=UEFI_PORTS, tags=['uefi'])
def uefi_shell_key(t):
    """With a UEFI Shell at the root of the boot volume U boots it [intent];
    without one the key is not offered."""
    m, _ = uefi_boot(t, tree_menu())
    t.check('UEFI Shell' not in screen_text(t.wait_text(m, 'ARROWS')), '[intent] not offered without a shell')
    m.stop()
    # The probe stands in for the shell, and with no arguments reads its
    # commands from the volume's root.
    m, _ = uefi_boot(t, tree_menu(), files={'/' + t.port.shell_name.upper(): HostFile(probe_file(t)),
                                            '/efiprobe.cmd': b'name:fakeshell'})
    t.check('U UEFI Shell' in screen_text(t.wait_text(m, 'ARROWS')), '[intent] offered, found regardless of case')
    m.press('u')
    m.wait_for(t.kernel_channel, 'K name=fakeshell', 60)


@check(ports=UEFI_PORTS, tags=['uefi', 'acpi'])
def uefi_firmware_logo(t):
    """firmware_logo: yes clears the BGRT's displayed bit, for the OS to draw
    the logo again, and centres the logo for the resolution the kernel gets;
    without it the table is left as the firmware made it (CONFIG.md), as it
    is for a kernel whose screen Limine keeps, one asking for the terminal's
    parameters [intent]. The table's checksum stays right [spec: ACPI 6.6
    5.2.23]."""
    for logo, variant, resolution, restored in ((False, 'noflanterm', None, False),
                                                (True, 'noflanterm', None, True),
                                                (True, 'noflanterm', '800x600', True),
                                                (True, 'full', None, False)):
        what = f'firmware_logo {logo}, {variant}, resolution {resolution}'
        config = 'timeout: 0\n' + ('firmware_logo: yes\n' if logo else '') + entry(
            'Test', path='boot():/boot/kernel.elf', cmdline='x', extra=f'resolution: {resolution}' if resolution else '')
        m, _ = boot_entry(t, config, kernel_variant=variant)
        r = t.kernel_report(m)
        check_report_clean(t, r)
        t.require(r['bgrt.status'] != 'none', 'the firmware made a BGRT')
        t.expect_eq(int(r['bgrt.status']) & 1, int(not restored), f'{what}: the displayed bit')
        t.expect_eq(r['bgrt.checksum_ok'], '1', f'{what}: the checksum')
        if restored:
            fb_w, fb_h = (int(x) for x in r['framebuffer.0'].split()[0].split('x')[:2])
            if resolution:
                t.expect_eq(f'{fb_w}x{fb_h}', resolution, f'{what}: the resolution')
            img_w, img_h = (int(x) for x in r['bgrt.image'].split())
            t.expect_eq(tuple(int(x) for x in r['bgrt.offset'].split()), ((fb_w - img_w) // 2, (fb_h - img_h) // 2),
                        f'{what}: centred in {fb_w}x{fb_h}')
        m.stop()


@check(ports=UEFI_PORTS, tags=['uefi', 'hostile'])
def uefi_watchdog_not_disabled(t):
    """Firmware that will not disable its watchdog, here made so by the EFI
    probe that starts Limine, has Limine warn and go on [intent]."""
    from cases.chainload import probe_then_limine
    m = probe_then_limine(t, 'name:wd wdfail', 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf',
                                                                       cmdline='x'),
                          {'/boot/kernel.elf': HostFile(kernel_file(t, 'full'))})
    r = t.kernel_report(m)
    check_report_clean(t, r)
    # The fallback terminal can place each character with a cursor move.
    serial = re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', m.stream('serial').text())
    t.check('WARNING: Failed to disable watchdog timer!' in serial, 'the warning')
