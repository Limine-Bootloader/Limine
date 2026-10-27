from harness.api import check
from harness.media import HostFile, Tree
from harness.ports import UEFI_PORTS
from harness.report import unescape
from cases._common import entry, wait_panic

EFI_ARCH = {'uefi-ia32': 'ia32', 'uefi-x86-64': 'x86_64', 'uefi-aarch64': 'aarch64',
            'uefi-riscv64': 'riscv64', 'uefi-loongarch64': 'loongarch64'}


def probe(t):
    return HostFile(t.env.payload('efi', EFI_ARCH[t.port.name], 'efiprobe.efi'))


def probe_then_limine(t, commands, config, files=None, **machine):
    """Boot the EFI probe, which runs commands and then starts Limine with
    config: firmware states QEMU does not give, made by the probe first."""
    tree = Tree()
    tree.add('/EFI/BOOT/' + t.port.efi_name, probe(t))
    tree.add('/efiprobe.cmd', f'{commands} chain:\\EFI\\limine\\{t.port.efi_name}\n')
    tree.add('/EFI/limine/' + t.port.efi_name, HostFile(t.env.bin(t.port.efi_name)))
    tree.add('/EFI/limine/limine.conf', config)
    for path, data in (files or {}).items():
        tree.add(path, data)
    return t.boot(t.boot_disk(tree), **machine)


def boot_config(t, config, files=None, efi_path=None, conf_path='/boot/limine.conf', **machine):
    tree = Tree()
    for path, data in (files or {}).items():
        tree.add(path, data)
    t.limine_files(config, tree, efi_path=efi_path, conf_path=conf_path)
    disk = t.boot_disk(tree)
    return t.boot(disk, **machine), disk


def probe_report(t, m, timeout=60):
    return t.kernel_report(m, timeout=timeout, begin='B EFIPROBE')


@check(ports=UEFI_PORTS, tags=['chainload'])
def efi_chainload(t):
    """An EFI image runs with the cmdline as its load options, the partition
    as its device, and its path from that device's root."""
    config = 'timeout: 0\n' + entry('Probe', protocol='efi', path='boot():/EFI/probe/probe.efi',
                                    cmdline='name:first info')
    m, disk = boot_config(t, config, {'/EFI/probe/probe.efi': probe(t)})
    r = probe_report(t, m)
    t.expect_eq(r['options'], 'name:first info', 'load options carry the cmdline')
    t.expect_eq(r.int('options_size'), (len('name:first info') + 1) * 2, 'load options size (UTF-16, terminated)')
    t.expect_eq(r['options_last_unit'], '0', 'terminator inside LoadOptionsSize')
    t.expect_eq(unescape(r['file_path']), '\\EFI\\probe\\probe.efi',
                'FilePath rooted at the partition, like those of images firmware loads')
    device = unescape(r['device_path'])
    t.check(disk.parts[0].guid.lower() in device.lower(),
            f'device path {device} is the boot partition {disk.parts[0].guid}')
    parent = unescape(r['parent_file_path'])
    t.check(parent.upper().endswith('\\EFI\\BOOT\\' + t.port.efi_name), f'parent image is Limine: {parent}')


def shown_utf16(text, extra_units=()):
    """How the probe shows UTF-16: printable ASCII but the backslash as is,
    each other code unit escaped."""
    data = text.encode('utf-16-le')
    units = [int.from_bytes(data[i:i + 2], 'little') for i in range(0, len(data), 2)] + list(extra_units)
    return ''.join(chr(u) if 0x20 <= u < 0x7f and u != 0x5c else f'\\u{u:04x}' for u in units), len(units)


@check(ports=UEFI_PORTS, tags=['chainload', 'hostile'])
def efi_chainload_unicode(t):
    """A cmdline past ASCII reaches the image as UTF-16 load options, a
    character past the BMP as a surrogate pair, each maximal ill-formed part
    of the UTF-8 as one U+FFFD, as Unicode 18.0.0 section 3.9.6 recommends;
    so does a path holding such a character, in the image's file path
    [intent]."""
    from cases.fs import patch_long_name
    bad = [(b'\xc0\xaf', 2), (b'\xed\xa0\x80', 3), (b'\xf4\x90\x80\x80', 4), (b'\xe6\x97', 1)]
    cmdline = 'name:utf info é日😀'.encode() + b''.join(b' ' + seq for seq, _ in bad)
    config = ('timeout: 0\n' + entry('Probe', protocol='efi', path='boot():/EFI/probe/😀-probe.efi',
                                     cmdline='@CMDLINE@')).encode().replace(b'@CMDLINE@', cmdline)
    tree = Tree()
    tree.add('/EFI/probe/QQ-probe.efi', probe(t))
    t.limine_files(config, tree)
    disk = t.boot_disk(tree)
    patch_long_name(disk.path, 'QQ-pr', [0xd83d, 0xde00])
    m = t.boot(disk)
    r = probe_report(t, m)
    extra = []
    for _, replacements in bad:
        extra += [0x20] + [0xfffd] * replacements
    options, units = shown_utf16('name:utf info é日😀', extra)
    t.expect_eq(r['options'], options, 'the cmdline in UTF-16')
    t.expect_eq(r.int('options_size'), (units + 1) * 2, 'load options size (UTF-16, terminated)')
    t.expect_eq(r['file_path'], shown_utf16('\\EFI\\probe\\😀-probe.efi')[0], 'the path in UTF-16')


@check(ports=UEFI_PORTS, tags=['chainload'])
def efi_chainload_aliases(t):
    """'uefi' and 'efi_chainload' alias 'efi', 'image_path' aliases 'path'."""
    for proto, key in (('uefi', 'path'), ('efi_chainload', 'image_path'), ('efi', 'image_path')):
        config = 'timeout: 0\n' + entry('Probe', protocol=proto, cmdline=f'name:{proto}',
                                        extra=f'{key}: boot():/probe.efi')
        m, _ = boot_config(t, config, {'/probe.efi': probe(t)})
        r = probe_report(t, m)
        t.expect_eq(r['name'], proto, f'{proto} with {key} boots the image')
        m.stop()


@check(ports=UEFI_PORTS, tags=['chainload'])
def efi_chainload_no_cmdline(t):
    """With no cmdline, an image gets no load options at all; it then reads
    its commands from \\efiprobe.cmd on its volume."""
    config = 'timeout: 0\n' + entry('Probe', protocol='efi', path='boot():/probe.efi')
    m, _ = boot_config(t, config, {'/probe.efi': probe(t), '/efiprobe.cmd': 'name:nocmd info\n'})
    r = probe_report(t, m)
    t.expect_eq(r['name'], 'nocmd', 'probe ran')
    t.expect_eq(r['options'], '(null)', 'no load options')
    t.expect_eq(r.int('options_size'), 0, 'load options size 0')


@check(ports=UEFI_PORTS, tags=['chainload'])
def efi_chainload_resolution(t):
    """The resolution option sets the video mode the image starts in."""
    config = 'timeout: 0\n' + entry('Probe', protocol='efi', path='boot():/probe.efi',
                                    cmdline='gop', extra='resolution: 800x600')
    m, _ = boot_config(t, config, {'/probe.efi': probe(t)})
    r = probe_report(t, m)
    t.expect_eq(r['gop.mode'], '800x600', 'GOP mode')


@check(ports=UEFI_PORTS, tags=['chainload'])
def efi_chainload_return(t):
    """When the image returns, Limine exits too, handing its status and exit
    data to whatever started Limine (here, another probe)."""
    limine_dir_conf = 'timeout: 0\n' + entry('Probe', protocol='efi', path='boot():/inner.efi',
                                             cmdline='name:inner exit:1234@returned_by_inner')
    tree = Tree()
    tree.add('/EFI/BOOT/' + t.port.efi_name, probe(t))
    tree.add('/efiprobe.cmd', f'name:outer chain:\\EFI\\limine\\{t.port.efi_name}\n')
    tree.add('/EFI/limine/' + t.port.efi_name, HostFile(t.env.bin(t.port.efi_name)))
    tree.add('/EFI/limine/limine.conf', limine_dir_conf)
    tree.add('/inner.efi', probe(t))
    disk = t.boot_disk(tree)
    m = t.boot(disk)
    ch = t.kernel_channel
    m.wait_for(ch, 'K chain.exit_data=', 90)
    m.wait_for(ch, 'E DONE', 30, start=m.stream(ch).text().rfind('K chain.exit_data='))
    text = m.stream(ch).text()
    from harness.report import Report
    outer = Report(text[text.rfind('K chain.status='):], begin='K chain.status=')
    t.expect_eq(outer['chain.status'], '1234', 'status passed through Limine')
    t.expect_eq(outer['chain.exit_data'], 'returned by inner', 'exit data passed through Limine')


@check(ports=UEFI_PORTS, tags=['chainload'])
def efi_chainload_errors(t):
    config = 'timeout: 0\n' + entry('Probe', protocol='efi', path='boot():/missing.efi')
    m, _ = boot_config(t, config)
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'efi: Failed to open image with path `boot():/missing.efi`. Is the path correct?',
                'missing image')
    m.stop()
    config = 'timeout: 0\n' + entry('Probe', protocol='efi')
    m, _ = boot_config(t, config)
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'efi: Image path not specified', 'no path')


# BIOS chainloading. The boot record payload reports, on port 0xe9, its name,
# the registers it was entered with, the entry DS:SI points at, the partition
# table at 0x7be, and 16 bytes at 0x9c00 (0x2000 into what was loaded).

from harness.media import Disk, Part, Raw, FREEBSD_BOOT_TYPE
from harness.ports import Drive
from harness.report import Report


def bootrec(t, name):
    data = bytearray(open(t.env.payload('bios', 'bootrec.bin'), 'rb').read())
    off = data.find(b'BOOTREC_')
    data[off:off + 8] = name.encode().ljust(8, b'_')[:8]
    return bytes(data)


def bios_disks(t):
    """Disk 1 boots Limine and has a second partition holding a boot record;
    disk 2 is MBR with its own MBR code, two primaries and a logical one;
    disk 3 is GPT with a plain partition and a freebsd-boot one."""
    d2 = Disk('mbr', 16, mbr_id=0x1234abcd, parts=[
        Part(size_mb=2, fs=Raw(bootrec(t, 'D2P1')), type='83'),
        Part(size_mb=2, fs=Raw(bootrec(t, 'D2P2')), type='83'),
        Part(size_mb=8, type='5'),
        Part(size_mb=2, fs=Raw(bootrec(t, 'D2P5')), type='83', logical=True),
    ])
    d2.build(t.path('d2.img'), t.workdir)
    t.require([p.number for p in d2.parts] == [1, 2, 3, 5], 'disk 2 layout')
    with open(t.path('d2.img'), 'r+b') as f:
        f.write(bootrec(t, 'D2MBR')[:440])
    marker = bytes(range(0x40, 0x50))
    blob = bytearray(bootrec(t, 'D3BSD')) + bytes(0x2000 - 512) + marker
    d3 = Disk('gpt', 16, disk_guid='5D1A8E0C-6A3F-4C8B-9E21-0123456789AB', parts=[
        Part(size_mb=2, fs=Raw(bootrec(t, 'D3P1')), guid='0F2C5B9A-1111-4E2D-8C3B-ABCDEF012345'),
        Part(size_mb=2, fs=Raw(bytes(blob)), type=FREEBSD_BOOT_TYPE),
        Part(size_mb=2),
    ])
    d3.build(t.path('d3.img'), t.workdir)
    return d2, d3, marker


def boot_bios_chainload(t, entry_opts, protocol='bios', boot_iso=False):
    d2, d3, marker = bios_disks(t)
    config = 'timeout: 0\n' + entry('Chain', protocol=protocol, extra=entry_opts)
    tree = Tree()
    t.limine_files(config, tree)
    if boot_iso:
        iso = t.iso(tree)
        d1 = Disk('mbr', 16, parts=[Part(size_mb=4, fs=Raw(bootrec(t, 'H1P1')), type='83')])
        d1.build(t.path('d1.img'), t.workdir)
        drives = [Drive(t.path('d1.img')), Drive(t.path('d2.img')), Drive(t.path('d3.img')),
                  Drive(iso.path, media='cdrom', bootindex=0)]
    else:
        d1 = t.boot_disk(tree, extra_parts=[Part(size_mb=4, fs=Raw(bootrec(t, 'D1P2')), type='83')])
        drives = [Drive(d1.path, bootindex=0), Drive(t.path('d2.img'), bootindex=1),
                  Drive(t.path('d3.img'), bootindex=2)]
    m = t.machine(drives=drives)
    return m, d2, d3, marker


def bootrec_report(t, m):
    m.wait_for('debugcon', 'E DONE', 60)
    return Report(m.stream('debugcon').text(), 'B BOOTREC')


def check_real_mode_entry(t, r, dl, si):
    """[spec] is the BIOS Boot Specification 1.01, 6.5.1; [intent] what
    Limine's spinup() sets up, mirroring MBR code handing over to a VBR."""
    t.expect_eq(f"{int(r['dx'], 16) & 0xff:02x}", dl, '[spec] DL is the drive')
    t.expect_eq((r['cs'], r['ip']), ('0000', '7c00'), '[spec] entered at 0000:7c00')
    t.expect_eq((r['ds'], r['es'], r['ss']), ('0000', '0000', '0000'), '[intent] segments zero')
    t.expect_eq(r['sp'], '7c00', '[intent] stack below the boot record')
    t.expect_eq(r['si'], si, '[intent] DS:SI on the partition entry, 0 for a whole drive')
    t.check(int(r['flags'], 16) & 0x200, f'[intent] interrupts enabled (flags {r["flags"]})')


def mbr_entry(table_hex, slot):
    return table_hex[slot * 32:(slot + 1) * 32]


@check(ports=['bios'], tags=['chainload'])
def bios_chainload_drive(t):
    """A whole drive: its MBR runs, with no partition entry."""
    m, *_ = boot_bios_chainload(t, 'drive: 2')
    r = bootrec_report(t, m)
    t.expect_eq(r['name'], 'D2MBR___', 'the MBR of drive 2 ran')
    check_real_mode_entry(t, r, '81', '0000')


@check(ports=['bios'], tags=['chainload'])
def bios_chainload_partitions(t):
    """A primary partition's boot record runs with DS:SI on its entry in a
    copy of the table at 0x7be, that entry alone marked active."""
    m, d2, *_ = boot_bios_chainload(t, 'drive: 2\npartition: 2')
    r = bootrec_report(t, m)
    t.expect_eq(r['name'], 'D2P2____', 'partition 2 boot record ran')
    check_real_mode_entry(t, r, '81', '07ce')
    table = r['table']
    entry2 = mbr_entry(table, 1)
    t.expect_eq(r['entry'], entry2, 'DS:SI points at slot 2 of the table')
    t.expect_eq(entry2[:2], '80', 'slot 2 active')
    t.expect_eq([mbr_entry(table, i)[:2] for i in (0, 2, 3)], ['00', '00', '00'], 'others inactive')
    start = int.from_bytes(bytes.fromhex(entry2[16:24]), 'little')
    t.expect_eq(start, d2.parts[1].start_sector, 'entry LBA is the partition start')
    m.stop()

    m, d2, *_ = boot_bios_chainload(t, 'drive: 2\npartition: 5', protocol='bios_chainload')
    r = bootrec_report(t, m)
    t.expect_eq(r['name'], 'D2P5____', 'logical partition 5 boot record ran')
    check_real_mode_entry(t, r, '81', '07be')
    entry = r['entry']
    t.expect_eq(entry[:2], '80', 'its entry active')
    start = int.from_bytes(bytes.fromhex(entry[16:24]), 'little')
    count = int.from_bytes(bytes.fromhex(entry[24:32]), 'little')
    t.expect_eq((start, count), (d2.parts[3].start_sector, d2.parts[3].sectors),
                'logical entry made absolute')


@check(ports=['bios'], tags=['chainload', 'partitions'])
def bios_chainload_partitions_past_disk_end(t):
    """[user] The boot record of a partition cut at its disk's end gets DS:SI
    on its entry as the table holds it, with the size it claims [intent]."""
    from cases.partitions import DISK_SECTORS, mbr_entry as part_entry, put_record
    image = bytearray(DISK_SECTORS * 512)
    put_record(image, 0, [part_entry(0x83, 2048, 4096), part_entry(0x83, 16384, 24576),
                          part_entry(0x05, 8192, 8192)])
    put_record(image, 8192, [part_entry(0x83, 63, 30000)])
    image[16384 * 512:16385 * 512] = bootrec(t, 'CUTP2')
    image[8255 * 512:8256 * 512] = bootrec(t, 'CUTP5')
    with open(t.path('cut.img'), 'wb') as f:
        f.write(image)
    for partition, name, si, extent in ((2, 'CUTP2___', '07ce', (16384, 24576)),
                                        (5, 'CUTP5___', '07be', (8255, 30000))):
        config = 'timeout: 0\n' + entry('Chain', protocol='bios', extra=f'drive: 2\npartition: {partition}')
        d1 = t.boot_disk(t.limine_files(config))
        m = t.machine(drives=[Drive(d1.path, bootindex=0), Drive(t.path('cut.img'), bootindex=1)])
        r = bootrec_report(t, m)
        t.expect_eq(r['name'], name, f'partition {partition} boot record ran')
        check_real_mode_entry(t, r, '81', si)
        entry_hex = r['entry']
        start = int.from_bytes(bytes.fromhex(entry_hex[16:24]), 'little')
        count = int.from_bytes(bytes.fromhex(entry_hex[24:32]), 'little')
        t.expect_eq((start, count), extent, f'partition {partition}: the entry start and size')
        m.stop()


@check(ports=['bios'], tags=['chainload', 'partitions'])
def bios_chainload_second_extended_partition(t):
    """[intent] With two extended partitions, the boot record of a logical
    partition in either gets DS:SI on its own entry, the later extended
    partition's logicals numbered after the earlier one's."""
    from cases.partitions import DISK_SECTORS, mbr_entry as part_entry, put_record
    image = bytearray(DISK_SECTORS * 512)
    put_record(image, 0, [part_entry(0x83, 2048, 4096), part_entry(0x05, 8192, 8192),
                          part_entry(0x05, 16384, 8192)])
    put_record(image, 8192, [part_entry(0x83, 63, 2000)])
    put_record(image, 16384, [part_entry(0x83, 63, 2000)])
    image[8255 * 512:8256 * 512] = bootrec(t, 'FIRST5')
    image[16447 * 512:16448 * 512] = bootrec(t, 'SECOND6')
    with open(t.path('two.img'), 'wb') as f:
        f.write(image)
    for partition, name, extent in ((5, 'FIRST5__', (8255, 2000)), (6, 'SECOND6_', (16447, 2000))):
        config = 'timeout: 0\n' + entry('Chain', protocol='bios', extra=f'drive: 2\npartition: {partition}')
        d1 = t.boot_disk(t.limine_files(config))
        m = t.machine(drives=[Drive(d1.path, bootindex=0), Drive(t.path('two.img'), bootindex=1)])
        r = bootrec_report(t, m)
        t.expect_eq(r['name'], name, f'partition {partition} boot record ran')
        check_real_mode_entry(t, r, '81', '07be')
        entry_hex = r['entry']
        start = int.from_bytes(bytes.fromhex(entry_hex[16:24]), 'little')
        count = int.from_bytes(bytes.fromhex(entry_hex[24:32]), 'little')
        t.expect_eq((start, count), extent, f'partition {partition}: its own entry')
        m.stop()


@check(ports=['bios'], tags=['chainload'])
def bios_chainload_default_drive(t):
    """Without a drive, the boot drive."""
    m, *_ = boot_bios_chainload(t, 'partition: 2')
    r = bootrec_report(t, m)
    t.expect_eq(r['name'], 'D1P2____', 'partition 2 of the boot drive')
    t.expect_eq(f"{int(r['dx'], 16) & 0xff:02x}", '80', '[spec] DL is the boot drive')


@check(ports=['bios'], tags=['chainload'])
def bios_chainload_default_drive_from_cd(t):
    """Booted from a CD, an omitted drive is the hard disk of the same index."""
    m, *_ = boot_bios_chainload(t, 'partition: 1', boot_iso=True)
    r = bootrec_report(t, m)
    t.expect_eq(r['name'], 'H1P1____', 'hard disk 1, partition 1')


@check(ports=['bios'], tags=['chainload'])
def bios_chainload_by_id(t):
    """mbr_id and gpt_uuid/gpt_guid pick the drive; partition still applies."""
    m, *_ = boot_bios_chainload(t, 'mbr_id: 1234ABCD\npartition: 1')
    r = bootrec_report(t, m)
    t.expect_eq(r['name'], 'D2P1____', 'mbr_id selects drive 2')
    m.stop()
    m, *_ = boot_bios_chainload(t, 'drive: 1\nmbr_id: 1234abcd\npartition: 1')
    r = bootrec_report(t, m)
    t.expect_eq(r['name'], 'D2P1____', 'mbr_id overrides drive')
    m.stop()
    m, d2, d3, _ = boot_bios_chainload(t, 'gpt_uuid: 5d1a8e0c-6a3f-4c8b-9e21-0123456789ab\npartition: 1')
    r = bootrec_report(t, m)
    t.expect_eq(r['name'], 'D3P1____', 'gpt_uuid selects drive 3')
    check_real_mode_entry(t, r, '82', '0000')
    m.stop()
    m, *_ = boot_bios_chainload(t, 'mbr_id: 1234abcd\ngpt_guid: 5D1A8E0C-6A3F-4C8B-9E21-0123456789AB\npartition: 1')
    r = bootrec_report(t, m)
    t.expect_eq(r['name'], 'D3P1____', 'gpt_guid overrides mbr_id')


@check(ports=['bios'], tags=['chainload'])
def bios_chainload_freebsd(t):
    """A freebsd-boot partition is loaded whole at 0x7c00, as FreeBSD's pmbr
    does."""
    m, d2, d3, marker = boot_bios_chainload(t, 'gpt_uuid: 5d1a8e0c-6a3f-4c8b-9e21-0123456789ab\npartition: 2')
    r = bootrec_report(t, m)
    t.expect_eq(r['name'], 'D3BSD___', 'gptboot ran')
    t.expect_eq(f"{int(r['dx'], 16) & 0xff:02x}", '82', '[intent] DL is the drive, as pmbr passes it')
    t.expect_eq(r['tail'], marker.hex(), 'whole partition loaded, not just a sector')


@check(ports=['bios'], tags=['chainload'])
def bios_chainload_errors(t):
    cases = [
        ('drive: 2\npartition: 300', 'bios: BIOS partition number outside range 0-256'),
        ('drive: 0', 'bios: BIOS drive number outside range 1-256'),
        ('drive: 9', 'bios: Specified drive/partition not found'),
        ('drive: 2\npartition: 3', 'bios: Specified drive/partition not found'),
        ('drive: 2\npartition: 4', 'bios: Specified drive/partition not found'),
        ('drive: 2\npartition: 6', 'bios: Specified drive/partition not found'),
        ('mbr_id: deadbeef', 'bios: No matching MBR ID found'),
        ('gpt_uuid: not-a-guid', 'bios: Malformed GUID'),
        ('gpt_uuid: 11111111-2222-3333-4444-555555555555', 'bios: No matching GPT drive for GPT_GUID found'),
        ('gpt_guid: 0F2C5B9A-1111-4E2D-8C3B-ABCDEF012345', 'bios: GPT_GUID is that of a partition, not a drive'),
        ('gpt_uuid: 5d1a8e0c-6a3f-4c8b-9e21-0123456789ab\npartition: 9', 'bios: Partition specified is not valid'),
        ('gpt_uuid: 5d1a8e0c-6a3f-4c8b-9e21-0123456789ab\npartition: 3', 'bios: Volume is not bootable'),
    ]
    for opts, message in cases:
        m, *_ = boot_bios_chainload(t, opts)
        msg, _ = wait_panic(t, m)
        t.expect_eq(msg, message, opts.replace('\n', ', '))
        m.stop()
