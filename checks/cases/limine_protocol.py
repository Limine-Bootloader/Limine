import calendar
import re

from harness.api import check
from harness.media import HostFile
from harness.ports import ALL_PORTS
from harness.report import fnv1a, unescape
from harness.screen import builtin_font
from cases._common import (boot_entry, boot_kernel, check_file, check_report_clean, entry,
                           firmware_type, kernel_file, limine_version, read, wait_panic)

DEFAULT_ANSI = '000000 aa0000 00aa00 aa5500 0000aa aa00aa 00aaaa aaaaaa'
DEFAULT_ANSI_BRIGHT = '555555 ff5555 55ff55 ffff55 5555ff ff55ff 55ffff ffffff'


@check(ports=ALL_PORTS, tags=['limine-protocol'])
def full_conformance(t):
    """Every feature requested at base revision 6, checked against the
    specification by the executable and against the boot media here."""
    mod = b'first module contents\n' * 100
    report, m, disk = boot_kernel(t, 'full', smp=4, cmdline='hello world',
                                  modules=[('/boot/mod1.bin', mod, 'first module')])
    check_report_clean(t, report)

    t.expect_eq(report['entry'], 'request', 'entered through the entry point request')
    t.expect_eq(report['base_revision.supported'], '1', 'base revision 6 supported')
    t.expect_eq(report.int('base_revision.loaded'), 6, 'loaded base revision')

    t.expect_eq(unescape(report['bootloader_info.name']), 'Limine', 'bootloader name')
    t.expect_eq(unescape(report['bootloader_info.version']), limine_version(t.env), 'bootloader version')
    t.expect_eq(unescape(report['cmdline']), 'hello world', 'command line')
    t.expect_eq(report.int('firmware_type'), firmware_type(t.port), 'firmware type')

    check_file(t, report, 'executable_file', path='/boot/kernel.elf',
               data=read(kernel_file(t, 'full')), string='hello world', disk=disk)
    t.expect_eq(report.int('module.count'), 1, 'module count')
    t.expect_eq(report.int('module.revision'), 2, 'module response revision')
    check_file(t, report, 'module.00', path='/boot/mod1.bin', data=mod, string='first module',
               disk=disk)

    t.expect_eq(report.int('mp.cpu_count'), 4, 'CPU count')
    t.expect_eq(report.int('mp.aps_started'), 3, 'APs started')

    # Responses this port does or does not owe.
    uefi = t.port.is_uefi
    t.expect_eq(report.flag('efi_system_table.response'), uefi, 'EFI system table response')
    t.expect_eq(report.flag('efi_memmap.response'), uefi, 'EFI memory map response')
    t.expect_eq(report.flag('keep_iommu.response'), t.port.is_x86, 'keep IOMMU response (x86 only)')
    if t.port.arch == 'riscv64':
        t.expect_eq(report.flag('riscv_bsp_hartid.response'), True, 'RISC-V BSP hart ID response')
        t.expect_eq(report.int('riscv_bsp_hartid'), report.int('mp.bsp_id', 16), 'BSP hart ID agrees with MP')
    # Firmware publishes a device tree on some platforms only (dtb_path checks
    # cover the response); none exists on x86.
    if t.port.is_x86:
        t.expect_eq(report.flag('dtb.response'), False, 'no device tree response on x86')
    t.expect_eq(report.flag('rsdp.response'), True, 'RSDP response')
    t.expect_eq(report.flag('smbios.response'), True, 'SMBIOS response')
    t.expect_eq(report.flag('framebuffer.response'), True, 'framebuffer response')
    t.expect_eq(report.flag('tpm_event_log.response'), False, 'no TPM, no event log response')

    t.expect_eq(report.int('paging_mode'), 0, 'paging mode the executable asked for')
    t.expect_eq(report.int('entropy.count'), 16, 'entropy values')
    t.expect_eq(report.int('framebuffer.revision'), 1, 'framebuffer response revision')

    if t.port.arch == 'riscv64' and 'riscv64.timebase_frequency' in report:
        t.expect_eq(report.int('tsc_frequency'), report.int('riscv64.timebase_frequency'),
                    'timer frequency against the device tree')

    # Without a wallpaper or theme options the terminal parameters are defaults.
    t.expect_eq(report['flanterm.0.canvas'], '0', 'no canvas without a wallpaper')
    t.expect_eq(report['flanterm.0.ansi'], DEFAULT_ANSI, 'default palette')
    t.expect_eq(report['flanterm.0.ansi_bright'], DEFAULT_ANSI_BRIGHT, 'default bright palette')
    t.expect_eq(report['flanterm.0.default'], '00000000 00aaaaaa 00555555 00ffffff', 'default colours')
    t.expect_eq(report['flanterm.0.font'], '8x16 spacing=1 scale=1x1 margin=0 rotation=0',
                'default font geometry')
    t.expect_eq(report['flanterm.0.font_hash'], fnv1a(builtin_font()), 'builtin font data')


# QEMU's RISC-V virt firmware keeps time with a virtual clock that ignores the
# RTC, so the firmware's GetTime is what Limine must agree with there.
@check(ports=[p for p in ALL_PORTS if p != 'uefi-riscv64'], tags=['limine-protocol'])
def date_at_boot(t):
    """The Date at Boot feature reports the RTC as a UNIX timestamp."""
    base = calendar.timegm((2001, 2, 3, 4, 5, 6))
    report, m, disk = boot_kernel(t, 'full', machine_kwargs={'extra': ['-rtc', 'base=2001-02-03T04:05:06']})
    check_report_clean(t, report)
    date = report.int('date_at_boot')
    t.check(base <= date <= base + 60, f'date at boot {date}, RTC started at {base}')


@check(ports=ALL_PORTS, tags=['limine-protocol'])
def base_revisions(t):
    """Each base revision is honoured, the tag answered, and what the revision
    governs (HHDM extent, identity map, table addresses, memory map types)
    holds; the executable checks the details. aarch64 supports 6 alone."""
    for rev in range(6):
        if t.port.arch == 'aarch64':
            m, _ = boot_entry(t, 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf'),
                              kernel_variant=f'rev{rev}')
            msg, _ = wait_panic(t, m)
            t.expect_eq(msg, f'limine: Base revision {rev} is no longer supported for aarch64 (minimum: 6)',
                        f'panic for base revision {rev}')
            m.stop()
            continue
        report, m, _ = boot_kernel(t, f'rev{rev}', smp=2)
        check_report_clean(t, report)
        t.expect_eq(report.flag('base_revision.supported'), True, f'rev{rev}: supported')
        t.expect_eq(report.int('base_revision.loaded'), rev, f'rev{rev}: loaded revision')
        t.expect_eq(report.flag('identity_map'), rev == 0, f'rev{rev}: identity map present')
        t.expect_eq(report.int('mp.aps_started'), 1, f'rev{rev}: AP started')
        m.stop()


@check(ports=ALL_PORTS, tags=['limine-protocol'])
def base_revision_unsupported(t):
    """A revision newer than the bootloader's: loaded with the newest it has,
    the third tag word left untouched, the second set to that revision."""
    report, m, _ = boot_kernel(t, 'rev7')
    check_report_clean(t, report)
    t.expect_eq(report.flag('base_revision.supported'), False, 'revision 7 reported unsupported')
    t.expect_eq(report['base_revision.tag'].split()[2], '7', 'third tag word left as 7')
    t.expect_eq(report.int('base_revision.loaded'), 6, 'loaded with base revision 6')
    t.expect_eq(report.flag('identity_map'), False, 'no identity map (revision 6 semantics)')


@check(ports=[p for p in ALL_PORTS if p != 'uefi-aarch64'], tags=['limine-protocol'])
def base_revision_absent(t):
    """No base revision tag means base revision 0."""
    report, m, _ = boot_kernel(t, 'norev')
    check_report_clean(t, report)
    t.expect_eq(report.int('base_revision.effective'), 0, 'treated as revision 0')
    t.expect_eq(report.flag('identity_map'), True, 'identity map present')


@check(ports=['uefi-aarch64'], tags=['limine-protocol'])
def base_revision_absent_aarch64(t):
    """Without a tag the executable asks for revision 0, refused on aarch64."""
    m, _ = boot_entry(t, 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf'),
                      kernel_variant='norev')
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'limine: Base revision 0 is no longer supported for aarch64 (minimum: 6)',
                'panic message')


def boot_variant(t, variant, extra_entry='', cmdline='x', modules=(), smp=1, cpu=None, machine=None,
                 extra=None, globals_='timeout: 0\n', files=None):
    """Boot a check executable variant with extra entry options; returns the
    machine."""
    mod_entries = []
    tree_files = dict(files or {})
    for mpath, data, mstring in modules:
        if data is not None:
            tree_files[mpath] = data
        mod_entries.append((f'boot():{mpath}', mstring))
    config = globals_ + entry('Test', path='boot():/boot/kernel.elf', cmdline=cmdline,
                              modules=mod_entries, extra=extra_entry)
    kwargs = {'smp': smp}
    if cpu is not None:
        kwargs['cpu'] = cpu
    if machine is not None:
        kwargs['machine_opts'] = machine
    if extra is not None:
        kwargs['extra'] = extra
    m, disk = boot_entry(t, config, files=tree_files, kernel_variant=variant, **kwargs)
    return m, disk


def expect_report(t, m, what):
    report = t.kernel_report(m)
    check_report_clean(t, report)
    return report


def expect_panic(t, m, message, what):
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, message, what)


@check(ports=ALL_PORTS, tags=['limine-protocol'])
def minimal_executable(t):
    """An executable with only a base revision tag boots, with no responses."""
    m, _ = boot_variant(t, 'minimal')
    report = expect_report(t, m, 'minimal')
    t.expect_eq(report.int('base_revision.loaded'), 6, 'loaded base revision')
    t.expect_eq(report['entry'], 'elf', 'entered at the ELF entry point')


@check(ports=ALL_PORTS, tags=['limine-protocol'])
def elf_entry_point(t):
    """Without an entry point request, execution starts at the ELF entry."""
    m, _ = boot_variant(t, 'noentry')
    report = expect_report(t, m, 'noentry')
    t.expect_eq(report['entry'], 'elf', 'entry')


@check(ports=ALL_PORTS, tags=['limine-protocol'])
def no_mp_request(t):
    """Without an MP request there is no MP response."""
    m, _ = boot_variant(t, 'nomp', smp=2)
    report = expect_report(t, m, 'nomp')
    t.expect_eq('mp.response' in report, False, 'no MP facts')


@check(ports=['bios', 'uefi-ia32', 'uefi-x86-64'], tags=['limine-protocol'])
def x2apic(t):
    """The MP request's x2APIC flag: honoured where the CPU has x2APIC."""
    m, _ = boot_variant(t, 'x2apic', smp=2, cpu='qemu64,+x2apic')
    report = expect_report(t, m, 'x2apic capable CPU')
    t.expect_eq(report.int('mp.flags', 16), 1, 'x2APIC flag in the response')
    t.expect_eq(report['x86.apic_mode'], 'x2apic', 'x2APIC mode in force')
    t.expect_eq(report.int('mp.aps_started'), 1, 'AP started')
    m.stop()
    m, _ = boot_variant(t, 'x2apic', smp=2, cpu='qemu64,-x2apic')
    report = expect_report(t, m, 'CPU without x2APIC')
    t.expect_eq(report.int('mp.flags', 16), 0, 'no x2APIC flag without x2APIC')
    t.expect_eq(report['x86.apic_mode'], 'xapic', 'xAPIC mode')
    m.stop()
    m, _ = boot_variant(t, 'full', smp=2, cpu='qemu64,+x2apic')
    report = expect_report(t, m, 'x2APIC capable CPU, flag clear')
    t.expect_eq(report.int('mp.flags', 16), 0, 'no x2APIC flag when not asked for')
    t.expect_eq(report['x86.apic_mode'], 'xapic', 'xAPIC mode when not asked for')


# Per architecture: the CPU model with every paging mode, one without the
# highest, and the protocol's number for that highest mode.
PAGING = {
    'x86-64': ('qemu64,+la57', 'qemu64,-la57', 1, '5level', '4level'),
    'aarch64': ('max', 'cortex-a72', 1, '5level', '4level'),
    'riscv64': ('rv64,sv57=on', 'rv64,sv57=off', 2, 'sv57', 'sv48'),
}


def paging_cpu(t, top):
    full, limited, *_ = PAGING[t.port.arch]
    return full if top else limited


@check(ports=[p for p in ALL_PORTS if p != 'uefi-loongarch64'], tags=['limine-protocol', 'paging'])
def paging_mode_requests(t):
    """The paging mode request picks the mode within what the executable and
    the CPU allow, and refuses to boot when the two do not meet."""
    full, limited, top, top_name, below_name = PAGING[t.port.arch]
    hhdm_5 = {'x86-64': 0xff00000000000000, 'aarch64': 0xfff0000000000000}

    m, _ = boot_variant(t, 'pagingtop', cpu=full)
    r = expect_report(t, m, 'top mode on a capable CPU')
    t.expect_eq(r.int('paging_mode'), top, 'top mode granted')
    t.expect_eq(r.int('paging_mode.hardware'), top, 'top mode in force')
    m.stop()

    m, _ = boot_variant(t, 'pagingtop', cpu=limited)
    r = expect_report(t, m, 'top mode on a CPU without it')
    t.expect_eq(r.int('paging_mode'), top - 1 if t.port.arch == 'riscv64' else 0, 'falls back')
    m.stop()

    m, _ = boot_variant(t, 'pagingrev0top', cpu=full)
    r = expect_report(t, m, 'revision 0 request for the top mode')
    t.expect_eq(r.int('paging_mode'), top, 'revision 0 request honoured')
    m.stop()

    m, _ = boot_variant(t, 'pagingonlytop', cpu=limited)
    expect_panic(t, m, "limine: Executable's minimum supported paging mode higher than maximum "
                       "allowable paging mode", 'min mode above what the CPU has')
    m.stop()

    m, _ = boot_variant(t, 'pagingbad', cpu=full)
    expect_panic(t, m, "limine: Executable's paging max_mode lower than min_mode", 'max below min')
    m.stop()

    m, _ = boot_variant(t, 'nopaging', cpu=full)
    r = expect_report(t, m, 'no paging mode request')
    default = 1 if t.port.arch == 'riscv64' else 0
    t.expect_eq(r.int('paging_mode.hardware'), default, 'default mode without a request')
    t.expect_eq('paging_mode' in r, False, 'no paging mode response')
    m.stop()

    m, _ = boot_variant(t, 'pagingcapped', cpu=full)
    r = expect_report(t, m, 'top mode asked for, the lowest the most allowed')
    t.expect_eq(r.int('paging_mode'), 0, "the executable's maximum caps the mode")
    m.stop()

    m, _ = boot_variant(t, 'pagingraised', cpu=full)
    r = expect_report(t, m, 'the lowest mode asked for, the top the least allowed')
    t.expect_eq(r.int('paging_mode'), top, "the executable's minimum raises the mode")
    m.stop()

    if t.port.arch == 'riscv64':
        m, _ = boot_variant(t, 'nopaging', cpu='rv64,sv48=off,sv57=off')
        r = expect_report(t, m, 'no request, a CPU with Sv39 alone')
        t.expect_eq(r.int('paging_mode.hardware'), 0, 'the default, Sv48, lowered to Sv39')
        m.stop()

    if t.port.arch in hhdm_5:
        m, _ = boot_variant(t, 'pagingtop', cpu=full)
        r = expect_report(t, m, 'top mode HHDM')
        t.check(r.hex('hhdm.offset') >= hhdm_5[t.port.arch],
                f'HHDM {r["hhdm.offset"]} lies in the 5-level higher half')
        m.stop()


@check(ports=[p for p in ALL_PORTS if p != 'uefi-loongarch64'], tags=['limine-protocol', 'paging'])
def paging_mode_config(t):
    """paging_mode, max_paging_mode and min_paging_mode bound the choice."""
    full, limited, top, top_name, below_name = PAGING[t.port.arch]

    m, _ = boot_variant(t, 'pagingtop', cpu=full, extra_entry=f'paging_mode: {below_name}')
    r = expect_report(t, m, 'paging_mode below the top')
    t.expect_eq(r.int('paging_mode'), top - 1, 'paging_mode caps the mode')
    m.stop()

    m, _ = boot_variant(t, 'pagingtop', cpu=full, extra_entry=f'max_paging_mode: {below_name.upper()}')
    r = expect_report(t, m, 'max_paging_mode, upper case')
    t.expect_eq(r.int('paging_mode'), top - 1, 'max_paging_mode caps the mode')
    m.stop()

    # Without a request the executable is taken to support up to the default
    # mode alone, so a minimum above that cannot be met.
    m, _ = boot_variant(t, 'nopaging', cpu=full, extra_entry=f'min_paging_mode: {top_name}')
    expect_panic(t, m, 'limine: Default paging mode lower than minimum allowable paging mode',
                 'min_paging_mode above the default, no request')
    m.stop()

    m, _ = boot_variant(t, 'pagingtop', cpu=full, extra_entry=f'min_paging_mode: {top_name}')
    r = expect_report(t, m, 'min_paging_mode at the top, executable allows it')
    t.expect_eq(r.int('paging_mode'), top, 'min_paging_mode met')
    m.stop()

    m, _ = boot_variant(t, 'full', cpu=limited, extra_entry=f'min_paging_mode: {top_name}')
    expect_panic(t, m, 'limine: User set MIN_PAGING_MODE greater than maximum supported paging mode',
                 'min_paging_mode the CPU lacks')
    m.stop()

    m, _ = boot_variant(t, 'full', cpu=full,
                        extra_entry=f'max_paging_mode: {below_name}\nmin_paging_mode: {top_name}')
    expect_panic(t, m, 'limine: MAX_PAGING_MODE is lower than MIN_PAGING_MODE', 'max below min')
    m.stop()

    m, _ = boot_variant(t, 'full', cpu=full, extra_entry='paging_mode: 6level')
    expect_panic(t, m, 'limine: Invalid MAX_PAGING_MODE: `6level`', 'unknown paging_mode')
    m.stop()

    m, _ = boot_variant(t, 'full', cpu=full, extra_entry='min_paging_mode: bogus')
    expect_panic(t, m, 'limine: Invalid MIN_PAGING_MODE: `bogus`', 'unknown min_paging_mode')
    m.stop()


@check(ports=['uefi-loongarch64'], tags=['limine-protocol', 'paging'])
def paging_mode_loongarch(t):
    """loongarch64 has 4-level paging alone; the option accepts only that."""
    m, _ = boot_variant(t, 'full', extra_entry='paging_mode: 4level')
    r = expect_report(t, m, '4level')
    t.expect_eq(r.int('paging_mode'), 0, 'mode')
    m.stop()
    m, _ = boot_variant(t, 'full', extra_entry='paging_mode: 5level')
    expect_panic(t, m, 'limine: Invalid MAX_PAGING_MODE: `5level`', '5level refused')


@check(ports=ALL_PORTS, tags=['limine-protocol'])
def duplicate_request(t):
    """Two requests with one ID: the bootloader refuses the executable."""
    m, _ = boot_variant(t, 'dupreq')
    expect_panic(t, m, 'limine: Conflict detected for request ID 0xf55038d8e2a1202f 0x279426fcf5f59740',
                 'conflicting requests')


@check(ports=ALL_PORTS, tags=['limine-protocol'])
def duplicate_base_revision(t):
    m, _ = boot_variant(t, 'duprev')
    expect_panic(t, m, 'limine: Duplicated base revision tag', 'two base revision tags')


@check(ports=ALL_PORTS, tags=['limine-protocol'])
def request_outside_markers(t):
    """A request before the start marker is not honoured."""
    m, _ = boot_variant(t, 'outside')
    r = expect_report(t, m, 'outside')
    t.expect_eq(r.flag('outside_request.response'), False, 'request outside the markers ignored')


@check(ports=ALL_PORTS, tags=['limine-protocol'])
def no_markers(t):
    """Without delimiters the whole image is searched."""
    m, _ = boot_variant(t, 'nomarkers')
    r = expect_report(t, m, 'nomarkers')
    t.expect_eq(r.flag('firmware_type.response'), True, 'requests found')


@check(ports=ALL_PORTS, tags=['limine-protocol'])
def stack_sizes(t):
    """The Stack Size request grows the stack; below 64KiB it changes nothing.
    (The executable checks the size it is owed is mapped below its stack.)"""
    for variant in ('stackbig', 'stacksmall'):
        m, _ = boot_variant(t, variant, smp=2)
        r = expect_report(t, m, variant)
        t.expect_eq(r.flag('stack_size.response'), True, f'{variant}: response')
        t.expect_eq(r.int('mp.aps_started'), 1, f'{variant}: AP started')
        m.stop()


@check(ports=ALL_PORTS, tags=['limine-protocol'])
def entropy_counts(t):
    """Entropy: zero values gives a NULL array; past the limit, the limit."""
    m, _ = boot_variant(t, 'entropy0')
    r = expect_report(t, m, 'entropy0')
    t.expect_eq(r.int('entropy.count'), 0, 'zero values')
    m.stop()
    m, _ = boot_variant(t, 'entropybig')
    r = expect_report(t, m, 'entropybig')
    t.expect_eq(r.int('entropy.count'), 4096, 'implementation limit of 4096 values')


AMDVI_ON = (1 << 0) | (1 << 2) | (1 << 12)
AMDVI_RUNNING = (1 << 3) | (1 << 4) | (1 << 7) | (1 << 8)
# Translation and queued invalidation enabled.
VTD_ON = (1 << 31) | (1 << 26)


@check(ports=['uefi-ia32', 'uefi-x86-64'], tags=['limine-protocol', 'iommu'])
def iommu_off_at_entry(t):
    """An IOMMU the firmware left on, here the EFI probe that starts Limine,
    is off at entry from base revision 5, unless the executable asks for the
    x86-64 Keep IOMMU feature [spec]: Intel VT-d DMA remapping, and the
    queued invalidation the probe also turns on [intent]; AMD-Vi, its command
    buffer and event log stopped first [intent]."""
    from cases.chainload import probe_then_limine
    for device, command in (('intel-iommu', 'vtdon'), ('amd-iommu', 'amdvion')):
        for variant, kept in (('nokeepiommu', False), ('full', True)):
            what = f'{device}, {variant}'
            m = probe_then_limine(t, f'name:iommu {command}', 'timeout: 0\n' + entry(
                'Test', path='boot():/boot/kernel.elf', cmdline='x'),
                {'/boot/kernel.elf': HostFile(kernel_file(t, variant))}, machine='q35', extra=['-device', device])
            r = expect_report(t, m, what)
            probe = m.stream('debugcon').text()
            if device == 'intel-iommu':
                on = re.search(r'K vtd=([0-9a-f]+)', probe)
                was = int(on.group(1), 16) & VTD_ON if on is not None else 0
                t.check(was >> 31, f'{what}: the probe turned DMA remapping on ({was:x})')
                t.expect_eq(int(r['x86.vtd.0.gsts'], 16) & VTD_ON, was if kept else 0, f'{what}: at entry')
            else:
                on = re.search(r'K amdvi=([0-9a-f]+) ', probe)
                t.check(on is not None and int(on.group(1), 16) & AMDVI_ON == AMDVI_ON,
                        f'{what}: the probe turned the IOMMU, its command buffer and its event log on')
                _, control, _, status = r['x86.amdvi.0'].split()
                t.expect_eq(int(control, 16) & AMDVI_ON, AMDVI_ON if kept else 0, f'{what}: control at entry')
                if not kept:
                    t.expect_eq(int(status, 16) & AMDVI_RUNNING, 0, f'{what}: nothing running at entry')
            m.stop()


@check(ports=['bios'], tags=['limine-protocol', 'vbe'])
def vbe_fallback_resolutions(t):
    """Without a resolution asked for and no EDID, or with one no mode has,
    VBE goes down its list, 1024x768, 800x600 and 640x480 at 32 bits per
    pixel, then at 24, then 16, to the first the card has room for
    [intent]."""
    for dev, resolution, want in (('VGA,edid=off', None, '1024x768x32'),
                                  ('VGA,edid=off', '1234x567x32', '1024x768x32'),
                                  ('VGA,edid=off,vgamem_mb=1', None, '640x480x24')):
        extra = f'resolution: {resolution}' if resolution else ''
        m, _ = boot_entry(t, 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x', extra=extra),
                          display_dev=dev)
        r = expect_report(t, m, dev)
        t.expect_eq(r['framebuffer.0'].split()[0], want, f'{dev}, resolution {resolution}')
        m.stop()


@check(ports=['bios'], tags=['limine-protocol'])
def tsc_frequency_from_pit(t):
    """With no ACPI, and so no PM timer, the TSC is timed against the PIT
    [intent]: the frequency given is the one the PM timer gives, to 10%."""
    freqs = []
    for opts in ([], ['acpi=off']):
        m, _ = boot_variant(t, 'full', machine=opts)
        r = expect_report(t, m, f'machine {opts}')
        t.expect_eq(r.int('rsdp.response'), 0 if opts else 1, f'machine {opts}: ACPI tables')
        freqs.append(r.int('tsc_frequency'))
        m.stop()
    pm, pit = freqs
    t.check(abs(pit - pm) <= pm // 10, f'PIT {pit} against PM timer {pm}')


@check(ports=['bios', 'uefi-ia32', 'uefi-x86-64'], tags=['limine-protocol'])
def entropy_cpu_instructions(t):
    """Entropy from the CPU's RDSEED, and from its RDRAND where it has no
    RDSEED, the strongest sources the protocol names [spec]: the values
    there, none zero, no two alike."""
    for cpu in ('qemu64,+rdseed', 'qemu64,+rdrand'):
        m, _ = boot_variant(t, 'full', cpu=cpu)
        r = expect_report(t, m, cpu)
        t.expect_eq(r.int('entropy.count'), 16, f'{cpu}: the values asked for')
        m.stop()


INTERNAL_REQUIRED = b'required internal module\n' * 40
INTERNAL_DOTDOT = b'reached through ./im/../im/\n' * 3
INTERNAL_ABSOLUTE = b'from the volume root'
INTERNAL_COMPRESSED = bytes(range(256)) * 64
CONFIG_MODULE = b'configuration module\n' * 9


def internal_module_files():
    import gzip
    return {
        '/boot/im/required.bin': INTERNAL_REQUIRED,
        '/boot/im/dotdot.bin': INTERNAL_DOTDOT,
        '/internal-abs.bin': INTERNAL_ABSOLUTE,
        '/boot/im/compressed.bin.gz': gzip.compress(INTERNAL_COMPRESSED, mtime=0),
    }


@check(ports=ALL_PORTS, tags=['limine-protocol'])
def internal_modules(t):
    """Internal modules resolve against the executable's directory, missing
    optional ones are skipped, compressed ones are inflated, and all of them
    come before the configuration's modules."""
    report, _, disk = boot_kernel(t, 'im1', files=internal_module_files(),
                                  modules=[('/boot/conf.bin', CONFIG_MODULE, 'config module')])
    check_report_clean(t, report)
    t.expect_eq(report.int('module.response'), 1, 'module response')
    t.expect_eq(report.int('module.count'), 5, 'four internal modules found, then the configured one')
    expected = [
        ('/boot/im/required.bin', INTERNAL_REQUIRED, 'internal required'),
        ('/boot/im/dotdot.bin', INTERNAL_DOTDOT, 'internal dotdot'),
        ('/internal-abs.bin', INTERNAL_ABSOLUTE, ''),
        ('/boot/im/compressed.bin.gz', INTERNAL_COMPRESSED, 'internal compressed'),
        ('/boot/conf.bin', CONFIG_MODULE, 'config module'),
    ]
    for i, (path, data, string) in enumerate(expected):
        check_file(t, report, f'module.{i:02}', path=path, data=data, string=string, disk=disk)


@check(ports=ALL_PORTS, tags=['limine-protocol'])
def internal_module_required_missing(t):
    m, _ = boot_entry(t, 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf'), kernel_variant='im2')
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'limine: Failed to open module with path `boot():/boot/im/absent.bin`. Is the path correct?',
                'a required internal module that is missing')


@check(ports=ALL_PORTS, tags=['limine-protocol'])
def internal_module_optional_missing(t):
    """With no module found, there is no module response (PROTOCOL.md)."""
    report, _, _ = boot_kernel(t, 'im3', modules=[])
    check_report_clean(t, report)
    t.expect_eq(report.int('module.response'), 0, 'no module response')


@check(ports=ALL_PORTS, tags=['limine-protocol'])
def internal_modules_need_rev1(t):
    """A revision 0 module request has no internal modules (PROTOCOL.md)."""
    report, _, disk = boot_kernel(t, 'im1rev0', files=internal_module_files(),
                                  modules=[('/boot/conf.bin', CONFIG_MODULE, 'config module')])
    check_report_clean(t, report)
    t.expect_eq(report.int('module.count'), 1, 'only the configured module')
    check_file(t, report, 'module.00', path='/boot/conf.bin', data=CONFIG_MODULE, string='config module', disk=disk)


@check(ports=['uefi-aarch64'], tags=['limine-protocol'])
def aarch64_entry_el(t):
    """With the firmware, and so Limine, at EL2, the executable is entered at
    EL2 with VHE where the CPU has it, and at EL1 otherwise, never at EL2
    without it (PROTOCOL.md); the APs, started over PSCI by SMC from the
    ACPI tables or the device tree, at the BSP's EL (the payload checks)."""
    for cpu, want, opts in (('cortex-a72', 1, []), ('max', 2, []), ('cortex-a72', 1, ['acpi=off']),
                            ('max', 2, ['acpi=off'])):
        report, m, _ = boot_kernel(t, smp=4, machine_kwargs={'cpu': cpu,
                                                            'machine_opts': ['virtualization=on'] + opts})
        check_report_clean(t, report)
        vhe = report.int('aarch64.vhe') != 0
        what = f'{cpu}{" without ACPI" if opts else ""}'
        t.expect_eq((report.int('aarch64.el'), vhe), (want, want == 2), f'[spec] {what}: EL, VHE')
        t.expect_eq(report.int('mp.aps_started'), 3, f'{what}: APs started')
        m.stop()


@check(ports=ALL_PORTS, tags=['limine-protocol'])
def randomise_memory(t):
    """randomise_memory: yes, or its alias randomize_memory, fills RAM with
    random data at boot (CONFIG.md): most usable memory reaches the executable
    holding every byte value. Not all of it does [observed]."""
    for option in ('randomise_memory', 'randomize_memory'):
        report, m, _ = boot_kernel(t, config_globals=f'timeout: 0\n{option}: yes\n', cmdline='sample-memory')
        check_report_clean(t, report)
        # The executable samples each usable entry about 64 times whatever its
        # size, so each sample is weighed by the pages it stands for.
        usable = [(int(base, 16), int(length, 16)) for base, length, kind in
                  (v.split() for v in report.indexed('memmap')) if kind == 'usable']
        random = total = 0
        for v in report.multi.get('memmap.usable_page', []):
            addr, distinct = v.split()
            addr = int(addr, 16)
            base, length = next(e for e in usable if e[0] <= addr < e[0] + e[1])
            pages = length // 4096
            weight = min(max(pages // 64, 1), pages - (addr - base) // 4096)
            total += weight
            random += weight if int(distinct) >= 250 else 0
        t.check(random * 2 > total > 0, f'[spec] {option}: {random} of {total} usable pages random, by sample')
        m.stop()


@check(ports=['bios', 'uefi-ia32', 'uefi-x86-64'], tags=['limine-protocol', 'paging'])
def x86_1gib_pages(t):
    """On a CPU with 1GiB pages the direct map may use them; every mapping the
    executable checks holds the same [intent]."""
    m, _ = boot_variant(t, 'full', cpu='qemu64,+pdpe1gb')
    expect_report(t, m, '1GiB pages')


@check(ports=['uefi-riscv64'], tags=['limine-protocol', 'paging'])
def riscv_svpbmt(t):
    """[spec] With Svpbmt, mappings take the page-based memory types the
    protocol asks of them, framebuffers NC (PROTOCOL.md), which the
    executable checks entry by entry; it learns of Svpbmt from the device
    tree, which only firmware without ACPI hands on."""
    m, _ = boot_variant(t, 'full', cpu='rv64,svpbmt=on', machine=('acpi=off',))
    r = expect_report(t, m, 'Svpbmt')
    t.expect_eq(r.int('riscv64.svpbmt'), 1, 'the executable found Svpbmt')
