import struct
import zlib

from harness.api import check
from harness.media import BASIC_DATA_TYPE, Disk, Fat, HostFile, Part, Tree
from harness.ports import ALL_PORTS, Drive
from cases._common import check_file, check_report_clean, entry, kernel_file, wait_panic

# Partition tables Limine has to work harder for: a GPT to be recovered from
# its backup (UEFI 2.11 5.3.2), a GPT header longer than the 92 bytes it
# defines, and chains of extended boot records that loop, run on, or point
# outside their partition. Each sits on a data disk next to the boot disk,
# a module on it found by label.

MiB = 1 << 20


def gpt_data_disk(t, name, label, data, spoil):
    disk = Disk('gpt', 32, parts=[Part(size_mb=16, fs=Fat(16, label=label, files={'/m.bin': data}),
                                       type=BASIC_DATA_TYPE)])
    path = t.path(name)
    disk.build(path, t.workdir)
    with open(path, 'r+b') as f:
        image = bytearray(f.read())
        spoil(image)
        f.seek(0)
        f.write(image)
    return path


def regrow_header(image, lba, size):
    """The GPT header at lba, made size bytes long, its CRC over all of them."""
    at = lba * 512
    struct.pack_into('<I', image, at + 12, size)
    struct.pack_into('<I', image, at + 16, 0)
    image[at + 92:at + size] = bytes(size - 92)
    struct.pack_into('<I', image, at + 16, zlib.crc32(bytes(image[at:at + size])))


def bad_signature(image):
    image[512:520] = b'EFI PARX'


def bad_crc(image):
    image[512 + 56] ^= 0xff


def grown_bad_crc(image):
    """A spoilt primary, and the disk grown past the backup it names."""
    bad_crc(image)
    image.extend(bytes(1 << 20))


def long_header(image):
    regrow_header(image, 1, 128)
    regrow_header(image, len(image) // 512 - 1, 128)


def mbr_entry(ptype, start, count):
    return struct.pack('<B3sB3sII', 0, b'\xfe\xff\xff', ptype, b'\xfe\xff\xff', start, count)


def put_record(image, lba, entries):
    sector = bytearray(512)
    for i, e in enumerate(entries):
        sector[446 + 16 * i:462 + 16 * i] = e or bytes(16)
    sector[510:512] = b'\x55\xaa'
    image[lba * 512:(lba + 1) * 512] = sector


PRIMARY, EXTENDED, EXT_SECTORS = 2048, 40960, 8192


def ebr_data_disk(t, name, label, data, ebrs):
    """An MBR disk: a FAT primary partition holding data, and an extended
    partition with the given extended boot records, {sector within the
    extended partition: entries}."""
    path = t.path(name)
    image = bytearray(32 * MiB)
    put_record(image, 0, [mbr_entry(0x0e, PRIMARY, 32768), mbr_entry(0x05, EXTENDED, EXT_SECTORS)])
    for rel, entries in ebrs.items():
        put_record(image, EXTENDED + rel, entries)
    with open(path, 'wb') as f:
        f.write(image)
    Fat(16, label=label, files={'/m.bin': data}).write_into(path, PRIMARY * 512, 32768 * 512, t.workdir)
    return path


def link(rel):
    return mbr_entry(0x05, rel, 1)


def boot_with_data_disks(t, disks, readonly=False):
    """Boot an entry with one module from each data disk, by label; return
    the report."""
    tree = Tree()
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
    mods = [(f'fslabel({label}):/m.bin', label) for label, _, _ in disks]
    t.limine_files('timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x', modules=mods), tree)
    boot = t.boot_disk(tree)
    # virtio, as the PC's IDE controllers take four disks in all.
    drives = [Drive(boot.path, bootindex=1)] + [Drive(path, kind='virtio', readonly=readonly)
                                                for _, _, path in disks]
    m = t.boot(drives=drives)
    r = t.kernel_report(m)
    check_report_clean(t, r)
    m.stop()
    return r


def check_modules(t, r, disks):
    from harness.report import fnv1a, unescape
    for i, (label, data, _) in enumerate(disks):
        t.expect_eq((unescape(r[f'module.{i:02}.string']), r[f'module.{i:02}.hash']), (label, fnv1a(data)),
                    f'{label}: the module loaded')


@check(ports=ALL_PORTS, tags=['fs', 'partitions', 'hostile'])
def gpt_recovery(t):
    """A GPT whose primary header is spoilt, by its signature or its CRC, is
    read from its backup at the end of the disk [spec]; a header longer than
    92 bytes is checked over all of them [spec]. On a disk grown past it, the
    backup the spoilt primary names is found where the end holds none
    [intent]. The disks are read-only, so that UEFI firmware cannot restore
    the primary first."""
    disks = []
    for label, spoil in (('NOSIG', bad_signature), ('BADCRC', bad_crc), ('LONGHDR', long_header),
                         ('GROWN', grown_bad_crc)):
        data = f'module on {label}\n'.encode() * 40
        disks.append((label, data, gpt_data_disk(t, f'{label}.img', label, data, spoil)))
    check_modules(t, boot_with_data_disks(t, disks, readonly=True), disks)


@check(ports=ALL_PORTS, tags=['fs', 'partitions', 'hostile'])
def ebr_chains(t):
    """Chains of extended boot records that loop back, run past 256 links,
    point outside the extended partition, or put the link before an empty
    entry end without a hang, the disk's other partitions still usable
    [intent]."""
    loop = {0: [b'', link(1)], 1: [b'', link(0)]}
    long_chain = {i: [b'', link(i + 1)] for i in range(300)}
    outside = {0: [b'', link(EXT_SECTORS + 100)]}
    empty_first = {0: [mbr_entry(0, 0, 0), link(1)], 1: [b'', b'']}
    disks = []
    for label, ebrs in (('LOOP', loop), ('LONGCHAIN', long_chain), ('OUTSIDE', outside), ('EMPTYFIRST', empty_first)):
        data = f'module beside {label}\n'.encode() * 40
        disks.append((label, data, ebr_data_disk(t, f'{label}.img', label, data, ebrs)))
    check_modules(t, boot_with_data_disks(t, disks), disks)


# A 16MiB data disk, for partitions that run past its end.
DISK_SECTORS = 32768


def past_end_disk(t, name, records, volumes):
    """An MBR disk of DISK_SECTORS sectors holding records {lba: entries}
    and, for each (lba, sectors, label, data), a FAT volume there with data
    in /m.bin, which fits on the disk whatever its partition claims."""
    path = t.path(name)
    image = bytearray(DISK_SECTORS * 512)
    for lba, entries in records.items():
        put_record(image, lba, entries)
    with open(path, 'wb') as f:
        f.write(image)
    for lba, sectors, label, data in volumes:
        Fat(12, label=label, files={'/m.bin': data}).write_into(path, lba * 512, sectors * 512, t.workdir)
    return path


def boot_with_data_disk(t, path, modules):
    """Boot an entry loading modules [(uri, string)], the disk at path being
    drive 2; return the machine."""
    tree = Tree()
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
    t.limine_files('timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x', modules=modules),
                   tree)
    boot = t.boot_disk(tree)
    return t.boot(drives=[Drive(boot.path, bootindex=1), Drive(path, kind='virtio')])


@check(ports=ALL_PORTS, tags=['fs', 'partitions', 'hostile'])
def mbr_partitions_past_disk_end(t):
    """[user] An MBR or EBR partition that starts on its disk but runs past
    the end is kept, cut at the end, also inside an extended partition that
    runs past it. Whether an entry in the last two slots of an EBR takes a
    number does not depend on the disk's end, and one in the first two slots
    is a partition wherever on the disk it lies, inside the extended
    partition or not. Each module is loaded by its partition's number."""
    layouts = {
        'PRIMARY': ({0: [mbr_entry(0x0e, 2048, 8192), mbr_entry(0x0e, 16384, 24576),
                         mbr_entry(0x0e, 45056, 4096), mbr_entry(0x0e, DISK_SECTORS, 1024)]},
                    {1: (2048, 8192), 2: (16384, 16384)}),
        # Slot 3 of the first EBR takes number 6, though it starts past the
        # disk's end.
        'EXTENDED': ({0: [mbr_entry(0x0e, 2048, 4096), mbr_entry(0x05, 8192, 40000)],
                      8192: [mbr_entry(0x0e, 63, 4000), mbr_entry(0x05, 8192, 30000),
                             mbr_entry(0x0e, 31000, 8000)],
                      16384: [mbr_entry(0x0e, 63, 29937)]},
                     {1: (2048, 4096), 5: (8255, 4000), 7: (16447, 16321)}),
        'OUTSIDE': ({0: [mbr_entry(0x0e, 2048, 4096), mbr_entry(0x05, 8192, 8192)],
                     8192: [mbr_entry(0x0e, 12000, 2000), mbr_entry(0x0e, 63, 10000)]},
                    {1: (2048, 4096), 5: (20192, 2000), 6: (8255, 10000)}),
    }
    for name, (records, parts) in layouts.items():
        volumes, modules, data = [], [], {}
        for n, (lba, sectors) in parts.items():
            label = f'{name[:8]}P{n}'
            data[n] = f'module on {name} partition {n}\n'.encode() * 40
            volumes.append((lba, sectors, label, data[n]))
            modules.append((f'hdd(2:{n}):/m.bin', label))
        m = boot_with_data_disk(t, past_end_disk(t, f'{name}.img', records, volumes), modules)
        r = t.kernel_report(m)
        check_report_clean(t, r)
        for i, (n, (_, label)) in enumerate(zip(parts, modules)):
            check_file(t, r, f'module.{i:02}', path='/m.bin', data=data[n], string=label, partition=n)
        m.stop()


@check(ports=ALL_PORTS, tags=['fs', 'partitions', 'hostile'])
def gpt_entry_past_disk_end(t):
    """[user] A GPT entry that starts on its disk but runs past the end is no
    partition, though a FAT volume lies at its start on the disk."""
    from cases.host_edges import entry as gpt_entry, spoil
    disk = Disk('gpt', 16, parts=[Part(size_mb=4, fs=Fat(12, label='GPTIN', files={'/m.bin': b'in\n' * 40}),
                                       type=BASIC_DATA_TYPE),
                                  Part(size_mb=4, fs=Fat(12, label='GPTCUT', files={'/m.bin': b'cut\n' * 40}),
                                       type=BASIC_DATA_TYPE)])
    path = t.path('gpt.img')
    disk.build(path, t.workdir)
    spoil(path, gpt_entry(1, end=disk.parts[1].start_sector + DISK_SECTORS))
    m = boot_with_data_disk(t, path, [('hdd(2:1):/m.bin', None), ('hdd(2:2):/m.bin', None)])
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'limine: Failed to open module with path `hdd(2:2):/m.bin`. Is the path correct?',
                'partition 1 opened, partition 2 not found')
