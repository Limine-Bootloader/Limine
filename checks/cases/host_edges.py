import os
import struct
import uuid
import zlib

from harness.api import test
from harness.media import BASIC_DATA_TYPE, BIOS_BOOT_TYPE, Disk, HostFile, Iso, Part, Tree
from cases.host import limine, read

# What the host tool refuses, and what it does with what it is not given
# whole: its options, the uninstall data it keeps, the partition tables it
# will not install into without --force, and the GPT of an isohybrid, which
# it turns into an MBR. Expectations come from its --help text; the messages,
# and the tables it refuses, are [intent]: only its code says them. None of
# these boot anything.

LINUX_FS_TYPE = '0fc63daf-8483-4772-8e79-3d69d8477de4'
GPT_FIELDS = {'first_usable': (40, '<Q'), 'last_usable': (48, '<Q'), 'disk_guid': (56, '16s'),
              'count': (80, '<I')}


def write(t, name, data):
    path = t.path(name)
    with open(path, 'wb') as f:
        f.write(data)
    return path


def mbr_disk(t, name):
    # sfdisk aligns a disk this small to sectors, not to 1MiB, unless told.
    disk = Disk('mbr', 4, parts=[Part(start=2048, size_mb=1, type='0c', bootable=True)])
    return disk.build(t.path(name), t.workdir)


def gpt_disk(t, name, sector_size=512):
    """Partition 1 a data one, partition 2 the BIOS boot one, with nothing in
    either."""
    disk = Disk('gpt', 8, sector_size=sector_size,
                parts=[Part(size_mb=2, type=BASIC_DATA_TYPE), Part(size_mb=1, type=BIOS_BOOT_TYPE)])
    return disk.build(t.path(name), t.workdir)


def gpt_rewrite(data, lba, entries=None, **fields):
    """Edit the GPT header at lba and its entry array in place, then make both
    CRCs right again. entries(array, entry_size) edits the array."""
    at = lba * 512
    for name, value in fields.items():
        off, fmt = GPT_FIELDS[name]
        struct.pack_into(fmt, data, at + off, value)
    array_lba, count, size = struct.unpack_from('<QII', data, at + 72)
    start = array_lba * 512
    if entries is not None:
        array = bytearray(data[start:start + count * size])
        entries(array, size)
        data[start:start + count * size] = array
    struct.pack_into('<I', data, at + 88, zlib.crc32(bytes(data[start:start + count * size])))
    header_size = struct.unpack_from('<I', data, at + 12)[0]
    struct.pack_into('<I', data, at + 16, 0)
    struct.pack_into('<I', data, at + 16, zlib.crc32(bytes(data[at:at + header_size])))


def spoil(path, entries=None, alternate=True, **fields):
    """The same edit to the primary GPT and, unless told not to, the
    alternate one."""
    with open(path, 'r+b') as f:
        data = bytearray(f.read())
        lbas = [1]
        if alternate:
            lbas.append(struct.unpack_from('<Q', data, 512 + 32)[0])
        for lba in lbas:
            gpt_rewrite(data, lba, entries, **fields)
        f.seek(0)
        f.write(data)


def entry(index, type_guid=None, start=None, end=None, like=None):
    """An edit of one entry of an array: a copy of another, then the type
    and range given."""
    def edit(array, size):
        at = index * size
        if like is not None:
            array[at:at + size] = array[like * size:(like + 1) * size]
        if type_guid is not None:
            array[at:at + 16] = uuid.UUID(type_guid).bytes_le
        if start is not None:
            struct.pack_into('<Q', array, at + 32, start)
        if end is not None:
            struct.pack_into('<Q', array, at + 40, end)
    return edit


def gpt_entries(data, lba=1):
    """(type, start, end) of the used entries of the GPT whose header is at
    lba."""
    array_lba, count, size = struct.unpack_from('<QII', data, lba * 512 + 72)
    out = []
    for i in range(count):
        at = array_lba * 512 + i * size
        if data[at:at + 16] != bytes(16):
            out.append((str(uuid.UUID(bytes_le=bytes(data[at:at + 16]))),) + struct.unpack_from('<QQ', data, at + 32))
    return out


def install(t, path, *args):
    return limine(t, 'bios-install', path, *args)


@test(ports=['bios'], tags=['host'])
def host_cli_edges(t):
    """Each command's --help, and the arguments each refuses."""
    code, out, err = limine(t)
    t.check(code != 0 and out.startswith('usage: '), f'no command: usage: {out!r}')
    code, out, err = limine(t, 'frobnicate')
    t.check(code != 0 and out.startswith('usage: '), f'an unknown command: usage: {out!r}')
    for args in (('help',), ('-h',)):
        code, out, err = limine(t, *args)
        t.check(code == 0 and 'Commands: ' in out, f'{args}: usage: {out!r}')
    code, out, err = limine(t, 'version', '--version-only')
    t.check(code == 0 and out.count('\n') == 1 and not out.startswith('Limine'), f'--version-only: {out!r}')
    for args, ok in ((('version', '--help'), True), (('version', '-h'), True), (('version', '--bogus'), False)):
        code, out, err = limine(t, *args)
        t.check((code == 0) == ok and out.startswith('usage: '), f'{args}: usage, exit {code}')
    for flag in ('--help', '-h'):
        code, out, err = limine(t, 'bios-install', flag)
        t.check(code == 0 and out.startswith('usage: '), f'bios-install {flag}: usage, exit {code}')

    missing = t.path('missing.img')
    code, out, err = limine(t, 'bios-install', '--force', '--force', missing)
    t.check(code != 0 and 'warning: --force already set.' in err and 'No such file or directory' in err,
            f'[intent] --force twice, then a device that is not there: {err!r}')
    code, out, err = limine(t, 'bios-install', '--uninstall', '--uninstall', '--uninstall-data-file=a',
                            '--uninstall-data-file=b', missing)
    t.check(code != 0 and 'warning: --uninstall already set.' in err
            and 'warning: --uninstall-data-file already set. Overriding...' in err,
            f'[intent] repeated options warned about: {err!r}')
    code, out, err = limine(t, 'bios-install', '--uninstall-data-file=', missing)
    t.check(code != 0 and 'Uninstall data file has a zero-length name' in err, f'[intent] empty name: {err!r}')
    disk = mbr_disk(t, 'mbr.img')
    before = read(disk)
    code, out, err = limine(t, 'bios-install', disk, '--bogus')
    t.check(code != 0 and out.startswith('usage: ') and read(disk) == before,
            f'[intent] an option after the device: usage, the disk untouched: {out!r}')
    code, out, err = limine(t, 'bios-install', write(t, 'tiny.img', bytes(100)))
    t.check(code != 0 and "Couldn't determine block size of device" in err, f'[intent] under a sector: {err!r}')


@test(ports=['bios'], tags=['host'])
def host_enroll_config_edges(t):
    """enroll-config's usage, and the hashes and files it refuses: a hash of
    any length but a BLAKE2b or BLAKE3 one, or with a non-hexadecimal digit, a
    file it cannot seek, or too small to hold a hash, or with no hash slot,
    or with a slot too short for the hash [intent]."""
    good = 'ab' * 64
    code, out, err = limine(t, 'enroll-config', '--help')
    t.check(code == 0 and out.startswith('usage: '), f'--help: {out!r}')
    for args in (('-x',), (), ('file',), ('--reset',), ('a', 'b', 'c')):
        code, out, err = limine(t, 'enroll-config', *args)
        t.check(code != 0 and out.startswith('usage: '), f'{args}: usage, exit {code}')
    slot = b'++CONFIG_B2SUM_SIGNATURE++' + b'0' * 128
    target = write(t, 'small-slot.bin', b'\0' * 64 + slot + b'\0' * 256)
    cases = [
        ((target, 'ab' * 63 + 'a'), 'must be 128 BLAKE2b or 256 BLAKE3 hexadecimal characters long'),
        ((target, 'g' * 128), 'contains a non-hexadecimal character'),
        ((t.path('missing.bin'), good), 'No such file or directory'),
        ((write(t, 'tiny.bin', b'x' * 100), good), 'Bootloader file too small (need at least 154 bytes)'),
        ((write(t, 'noslot.bin', b'x' * 4096), good), 'Checksum location not found'),
        ((target, 'cd' * 128), 'only has a 128-character config hash slot'),
    ]
    fifo = t.path('fifo')
    os.mkfifo(fifo)
    cases.append(((fifo, good), 'enroll_config(): fseek()'))
    for args, message in cases:
        code, out, err = limine(t, 'enroll-config', *args)
        t.check(code != 0 and message in err, f'{os.path.basename(args[0])}: {message}: {err!r}')
    code, out, err = limine(t, 'enroll-config', target, good)
    t.check(code == 0 and slot.replace(b'0' * 128, good.encode()) in read(target),
            f'a BLAKE2b hash fits that slot: {err!r}')


def uninstall_data(*entries, count=None):
    """An uninstall data file: its entry count, then each entry's location,
    size and bytes. An entry may be raw bytes, to cut it short."""
    out = struct.pack('<Q', len(entries) if count is None else count)
    for e in entries:
        out += e if isinstance(e, bytes) else struct.pack('<QQ', e[0], len(e[1])) + e[1]
    return out


@test(ports=['bios'], tags=['host'])
def host_uninstall_data_edges(t):
    """--uninstall with uninstall data that is missing, short, or claims more
    than it can hold refuses before writing anything; data that cannot be
    written back is reported, retried once, and fails the uninstall; an
    install whose uninstall data cannot be saved is undone [intent]."""
    disk = mbr_disk(t, 'mbr.img')
    before = read(disk)
    cases = [
        ('missing', None, 'No such file or directory'),
        ('empty', b'', 'load_uninstall_data(): fread()'),
        ('too-many', uninstall_data(count=300), 'too many entries (300 > 256)'),
        ('no-entry', uninstall_data(count=1), 'load_uninstall_data(): fread()'),
        ('no-size', uninstall_data(struct.pack('<Q', 0)), 'load_uninstall_data(): fread()'),
        ('huge', uninstall_data(struct.pack('<QQ', 0, 1 << 62)), 'load_uninstall_data(): malloc()'),
        ('short', uninstall_data(struct.pack('<QQ', 0, 16) + b'12345678'), 'load_uninstall_data(): fread()'),
        ('second-short', uninstall_data((0, b'abcd'), struct.pack('<QQ', 8, 16)), 'load_uninstall_data(): fread()'),
    ]
    for name, data, message in cases:
        path = t.path(f'{name}.bin') if data is None else write(t, f'{name}.bin', data)
        code, out, err = limine(t, 'bios-install', '--uninstall', f'--uninstall-data-file={path}', disk)
        t.check(code != 0 and message in err and read(disk) == before, f'{name}: {message}: {err!r}')

    short_disk = write(t, 'short.img', bytes(1024))
    path = write(t, 'past-end.bin', uninstall_data((1 << 20, b'x' * 16)))
    code, out, err = limine(t, 'bios-install', '--uninstall', f'--uninstall-data-file={path}', short_disk)
    t.check(code != 0 and 'Uninstall data index 0 failed to write, retrying...' in err
            and 'warning: Retry failed.' in err and 'Some data failed to be uninstalled correctly' in err,
            f'past the end of the device: retried once, then failed: {err!r}')
    if os.path.exists('/dev/full'):
        path = write(t, 'full.bin', uninstall_data((0, b'x' * 16)))
        code, out, err = limine(t, 'bios-install', '--uninstall', f'--uninstall-data-file={path}', '/dev/full')
        t.check(code != 0 and 'Device cache flush failure. Uninstall may be incomplete.' in err,
                f'a device that takes no writes: {err!r}')
        path = write(t, 'full2.bin', uninstall_data((0, b'x' * 16), (4096, b'y' * 16)))
        code, out, err = limine(t, 'bios-install', '--uninstall', f'--uninstall-data-file={path}', '/dev/full')
        t.check(code != 0 and 'Uninstall data index 1 failed to write, retrying...' in err,
                f'and a second block, which first has to write the first back: {err!r}')

    for name, target, message in (('unopenable', t.path('no-such-dir/undo.bin'), 'No such file or directory'),
                                  ('full', '/dev/full', 'store_uninstall_data(): fwrite()')):
        if target == '/dev/full' and not os.path.exists(target):
            continue
        code, out, err = install(t, disk, f'--uninstall-data-file={target}')
        t.check(code != 0 and 'Storing uninstall data to file' in err and message in err
                and 'Install failed, undoing work...' in err and read(disk) == before,
                f'{name}: uninstall data not saved, so the install undone: {err!r}')


def mbr_with(entries=(), status=None, at=None):
    """A 1MiB disk with an MBR of the given (slot, type, start, sectors)
    entries, its signature, and optionally a status byte in a slot or bytes
    at an offset."""
    data = bytearray(1 << 20)
    for slot, ptype, start, count in entries:
        struct.pack_into('<B3sB3sII', data, 446 + 16 * slot, 0, b'\0\0\0', ptype, b'\0\0\0', start, count)
    if status is not None:
        data[446 + 16 * status[0]] = status[1]
    for off, value in (at or {}).items():
        data[off:off + len(value)] = value
    data[510:512] = b'\x55\xaa'
    return bytes(data)


@test(ports=['bios'], tags=['host'])
def host_mbr_checks(t):
    """Installing to an MBR, bios-install wants the boot signature, status
    bytes that are 0 or 0x80, no partition starting below sector 63, and no
    filesystem where the boot code goes; --force installs regardless,
    putting right what it can [intent]."""
    blank = write(t, 'blank.img', bytes(1 << 20))
    code, out, err = install(t, blank)
    t.check(code != 0 and 'Could not determine if the device has a valid partition table' in err
            and read(blank) == bytes(1 << 20), f'no signature: refused, untouched: {err!r}')
    code, out, err = install(t, blank, '--force')
    data = read(blank)
    t.check(code == 0 and data[510:512] == b'\x55\xaa' and data[446] == 0x80,
            f'--force: signature written, partition 1 made active: {err!r}')
    for slot in range(4):
        path = write(t, f'status{slot}.img', mbr_with([(slot, 0x0c, 2048, 1024)], status=(slot, 0x12)))
        code, out, err = install(t, path)
        t.check(code != 0 and 'Could not determine if the device has a valid partition table' in err,
                f'slot {slot + 1} status 0x12: refused: {err!r}')
        code, out, err = install(t, path, '--force')
        t.expect_eq((code, read(path)[446 + 16 * slot] & 0x7f), (0, 0), f'slot {slot + 1}: --force clears it')
        path = write(t, f'low{slot}.img', mbr_with([(slot, 0x0c, 1, 1024)]))
        before = read(path)
        code, out, err = install(t, path, '--force')
        t.check(code != 0 and "A partition's start sector is less than 63" in err and read(path) == before,
                f'slot {slot + 1} starting at sector 1: refused even with --force: {err!r}')
    hints = {'NTFS': {3: b'NTFS'}, 'FAT at 54': {54: b'FAT12'}, 'FAT at 82': {82: b'FAT32'},
             'FAT32 at 3': {3: b'FAT32'}, 'ext2': {1080: b'\x53\xef'}}
    for name, at in hints.items():
        path = write(t, f'{name.replace(" ", "-")}.img', mbr_with(at=at))
        code, out, err = install(t, path)
        t.check(code != 0 and 'Could not determine if the device has a valid partition table' in err,
                f'a filesystem signature ({name}) on the disk itself: refused: {err!r}')
        code, out, err = install(t, path, '--force')
        t.expect_eq(code, 0, f'{name}: --force installs: {err!r}')
    t.expect_eq(read(t.path('ext2.img'))[1080:1082], b'\0\0', '--force wiped the ext2 magic')
    path = write(t, 'twokib.img', mbr_with()[:2048])
    code, out, err = install(t, path)
    t.check(code != 0 and 'Install failed, undoing work...' in err and read(path) == mbr_with()[:2048],
            f'a disk too short for stage 2: {err!r}')
    path = write(t, 'short.img', mbr_with()[:600])
    code, out, err = install(t, path)
    t.check(code != 0 and 'Install failed, undoing work...' in err and read(path) == mbr_with()[:600],
            f'a disk too short to look for a filesystem on: {err!r}')
    if os.path.exists('/dev/full'):
        code, out, err = install(t, '/dev/full', '--force')
        t.check(code != 0 and 'Install failed, undoing work...' in err, f'a disk that takes no writes: {err!r}')


@test(ports=['bios'], tags=['host'])
def host_gpt_checks(t):
    """On GPT, the partition named for the boot code must be a number of one
    the table lists, wholly inside the usable range and clear of the GPT
    reserves and of every other partition, at least 32KiB, and hold no
    filesystem; --force waives the type and the filesystem only [intent]."""
    base = gpt_disk(t, 'base.img')
    pristine = read(base)
    reserve_end = 2 + 16384 // 512
    p2_start, p2_end = gpt_entries(pristine)[1][1:]
    last = len(pristine) // 512 - 1

    def case(name, args=(), entries=None, alternate=True, **fields):
        path = write(t, f'{name}.img', pristine)
        if entries is not None or fields:
            spoil(path, entries, alternate, **fields)
        before = read(path)
        code, out, err = install(t, path, *args)
        return code, err, read(path) == before, path

    for arg in ('abc', '0', '4294967296'):
        code, err, same, _ = case('badnum', (arg,))
        t.check(code != 0 and f'Invalid partition number `{arg}`' in err and same, f'index {arg!r}: {err!r}')
    code, err, same, _ = case('large', ('129',))
    t.check(code != 0 and 'Partition number is too large' in err and same, f'index 129 of 128: {err!r}')
    code, err, same, _ = case('nosuch', ('3',))
    t.check(code != 0 and 'No such partition: 3.' in err and same, f'an empty entry: {err!r}')
    refusals = [
        ('backwards', dict(entries=entry(1, end=p2_start - 1)), 'has ending LBA less than starting LBA'),
        ('reserve', dict(entries=entry(1, start=reserve_end - 1)), 'starts inside the GPT reserve'),
        ('unusable', dict(first_usable=p2_start + 1), 'lies outside the GPT usable range'),
        ('alternate', dict(entries=entry(1, end=last - 2), last_usable=last - 1, alternate=False),
         'ends inside the alternate GPT'),
        ('overlap', dict(entries=entry(1, start=p2_start - 8)), 'Partition 2 overlaps partition 1'),
        ('small', dict(entries=entry(1, end=p2_start + 31)), 'Partition 2 is smaller than 32KiB'),
    ]
    for name, edit, message in refusals:
        code, err, same, _ = case(name, ('--force',), **edit)
        t.check(code != 0 and message in err and same, f'{name}: refused even with --force: {err!r}')

    fs = write(t, 'fs.img', pristine)
    with open(fs, 'r+b') as f:
        f.seek(p2_start * 512 + 54)
        f.write(b'FAT16')
    before = read(fs)
    code, out, err = install(t, fs)
    t.check(code != 0 and 'contains\n       a recognised filesystem' in err and read(fs) == before,
            f'a FAT boot sector in the BIOS boot partition: refused: {err!r}')
    code, out, err = install(t, fs, '--force')
    t.expect_eq(code, 0, f'--force installs over it: {err!r}')
    code, err, same, _ = case('wrongtype', ('1', '--force'))
    t.check(code == 0 and 'Installing BIOS boot code to partition 1.' in err,
            f'--force installs to a partition of another type: {err!r}')

    spoilt = bytearray(pristine)
    spoilt[512 + 40] ^= 0xff
    code, out, err = install(t, write(t, 'primary-bad.img', bytes(spoilt)))
    t.check(code == 0 and 'Primary GPT did not verify; using the alternate header.' in err,
            f'a primary header that fails its CRC: the alternate used: {err!r}')
    code, out, err = install(t, write(t, 'moved.img', bytes(spoilt) + bytes(1 << 20)))
    t.check(code == 0 and 'using the alternate header' in err,
            f'and with the disk grown past it, the alternate the header names: {err!r}')

    path = gpt_disk(t, '4kn.img', sector_size=4096)
    data = bytearray(read(path))
    code, out, err = install(t, path)
    t.check(code == 0 and 'Logical block size of 4096 bytes.' in err, f'a GPT of 4096-byte blocks: {err!r}')
    for lba in (1, struct.unpack_from('<Q', data, 4096 + 32)[0]):
        at = lba * 4096
        struct.pack_into('<II', data, at + 12, 1024, 0)
        struct.pack_into('<I', data, at + 16, zlib.crc32(bytes(data[at:at + 1024])))
    code, out, err = install(t, write(t, '4kn-long.img', bytes(data)))
    t.check(code == 0 and 'Installing to GPT' in err, f'and with 1024-byte headers [spec: UEFI 2.11 5.3.2]: {err!r}')


def hybrid_iso(t, name):
    """A hybrid ISO as USAGE.md makes one, before bios-install: the GPT in it
    is the one xorriso writes."""
    tree = Tree()
    for f in ('limine-bios.sys', 'limine-bios-cd.bin', 'limine-uefi-cd.bin'):
        tree.add('/boot/' + f, HostFile(t.env.bin(f)))
    path = t.path(name)
    Iso(tree, bios_boot='/boot/limine-bios-cd.bin', uefi_boot='/boot/limine-uefi-cd.bin').build(path, t.workdir)
    return path


def chs(lba):
    cylinder = lba // (255 * 63)
    if lba > 63 * 255 * 1024 or cylinder >= 1024:
        return b'\xfe\xff\xff'
    sector = lba % 63 + 1
    return bytes([lba // 63 % 255, (cylinder >> 2) & 0xc0 | sector, cylinder & 0xff])


def fnv1a32(data):
    h = 2166136261
    for b in data:
        h = (h ^ b) * 16777619 & 0xffffffff
    return h or 1


MBR_TYPES = {'c12a7328-f81f-11d2-ba4b-00a0c93ec93b': 0xef, 'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7': 0x07,
             '48465300-0000-11aa-aa11-00306543ecac': 0xaf}


@test(ports=['bios'], tags=['host', 'iso'])
def host_isohybrid_conversion(t):
    """An isohybrid's GPT becomes an MBR of the same partitions, typed from a
    table of GPT types, CHS filled in, the disk ID derived from the disk GUID,
    and every GPT header, the alternate wherever it is, wiped; a GPT it cannot
    convert is left for a GPT install, which then wants a BIOS boot partition,
    unless the conversion has to be refused outright [intent]."""
    pristine = read(hybrid_iso(t, 'base.iso'))
    parts = gpt_entries(pristine)
    disk_guid = pristine[512 + 56:512 + 72]
    alternate = struct.unpack_from('<Q', pristine, 512 + 32)[0]
    blocks = len(pristine) // 512
    first_usable = struct.unpack_from('<Q', pristine, 512 + 40)[0]

    def headers(data, *lbas):
        return [lba for lba in lbas if data[lba * 512:lba * 512 + 8] == b'EFI PART']

    path = write(t, 'plain.iso', pristine)
    code, out, err = install(t, path)
    data = read(path)
    t.check(code == 0 and 'Conversion successful.' in err, f'converted: {err!r}')
    want = b''
    for i, (ptype, start, end) in enumerate(parts):
        status = 0x80 if i == 0 else 0
        want += (bytes([status]) + chs(start) + bytes([MBR_TYPES[ptype]]) + chs(end)
                 + struct.pack('<II', start, end - start + 1))
    t.expect_eq(data[446:446 + 16 * len(parts)].hex(), want.hex(), 'the MBR entries, partition 1 made active')
    t.expect_eq(struct.unpack_from('<I', data, 0x1b8)[0], fnv1a32(disk_guid), 'disk ID from the disk GUID')
    t.expect_eq(headers(data, 1, alternate), [], 'both GPT headers wiped')

    tail = pristine[-33 * 512:]
    grown = pristine + bytes((1 << 20) - len(tail)) + tail
    code, out, err = install(t, write(t, 'grown.iso', grown))
    t.check(code == 0 and headers(read(t.path('grown.iso')), 1, alternate, len(grown) // 512 - 1) == [],
            f'imaged onto a larger disk: the alternate the header names and the one at the end wiped: {err!r}')

    spoilt = bytearray(pristine)
    spoilt[512 + 40] ^= 0xff
    path = write(t, 'primary-bad.iso', bytes(spoilt))
    code, out, err = install(t, path)
    t.check(code == 0 and 'using the alternate header' in err and 'Conversion successful.' in err
            and headers(read(path), 1, alternate) == [], f'converted from the alternate GPT: {err!r}')

    def five(array, size):
        for i in range(len(parts), 5):
            entry(i, like=0)(array, size)

    gap1 = len(parts) - 1
    refusals = [
        ('flag', ('--no-gpt-to-mbr-isohybrid-conversion',), {}, None),
        ('five', (), dict(entries=five), 'GPT contains more than 4 partitions, will not convert.'),
        ('backwards', (), dict(entries=entry(gap1, end=parts[gap1][1] - 1)),
         f'Partition {gap1 + 1} ends before it starts, will not convert GPT.'),
        ('unusable', (), dict(entries=entry(0, start=first_usable - 1)),
         'Partition 1 lies outside the GPT usable range, will not convert GPT.'),
        ('linux', (), dict(entries=entry(1, type_guid=LINUX_FS_TYPE)),
         'Cannot convert partition type for partition 2, will not convert GPT.'),
        ('past', (), dict(entries=entry(gap1, end=blocks + 50), last_usable=blocks + 100, alternate=False),
         f'Partition {gap1 + 1} ends past the device, will not convert GPT.'),
        ('many', (), dict(count=300, first_usable=96, alternate=False),
         'GPT declares more than 256 partition entries, will not convert GPT.'),
    ]
    for name, args, edit, message in refusals:
        path = write(t, f'{name}.iso', pristine)
        if edit:
            spoil(path, **edit)
        code, out, err = install(t, path, *args)
        t.check(code != 0 and 'Conversion successful.' not in err and (message is None or message in err)
                and 'no BIOS boot partition specified or' in err,
                f'{name}: not converted, then no BIOS boot partition: {err!r}')

    for name, edit, message in (
            ('low', dict(entries=entry(0, start=40), count=56, first_usable=34, alternate=False),
             "A partition's start sector is less than 63, aborting."),
            ('alt-usable', dict(last_usable=alternate, alternate=False),
             'GPT places an alternate header inside its usable range, aborting.')):
        path = write(t, f'{name}.iso', pristine)
        spoil(path, **edit)
        before = read(path)
        code, out, err = install(t, path)
        t.check(code != 0 and message in err and read(path) == before, f'{name}: refused, untouched: {err!r}')


def header_edit(pristine, edit, fix_crc=True):
    """The disk with edit(data, header offset, header lba) applied to both GPT
    headers, each CRC made right again unless told not to."""
    data = bytearray(pristine)
    for lba in (1, struct.unpack_from('<Q', data, 512 + 32)[0]):
        at = lba * 512
        edit(data, at, lba)
        if fix_crc:
            size = min(struct.unpack_from('<I', data, at + 12)[0], 512)
            struct.pack_into('<I', data, at + 16, 0)
            struct.pack_into('<I', data, at + 16, zlib.crc32(bytes(data[at:at + size])))
    return bytes(data)


def put(fmt, off, value):
    return lambda data, at, lba: struct.pack_into(fmt, data, at + off, value)


@test(ports=['bios'], tags=['host'])
def host_gpt_verify(t):
    """A GPT is used only when a header passes UEFI 2.11 section 5.3.2's
    checks, signature, header CRC, MyLBA and entry array CRC, and holds
    together: revision 1.0, a header no shorter than 92 bytes and no longer
    than a block, entries of 128 x 2^n bytes, an array of some entries, at
    most 1MiB, outside the usable range and on the medium. Failing on both
    headers, the disk is read as the MBR its protective one is, whose
    partition starts at sector 1 [intent]."""
    pristine = read(gpt_disk(t, 'base.img'))
    variants = {
        'revision': header_edit(pristine, put('<I', 8, 0x00010001)),
        'short header': header_edit(pristine, put('<I', 12, 91)),
        'long header': header_edit(pristine, put('<I', 12, 513)),
        'MyLBA': header_edit(pristine, lambda data, at, lba: struct.pack_into('<Q', data, at + 24, lba + 1)),
        'header CRC': header_edit(pristine, lambda data, at, lba: data.__setitem__(at + 40, data[at + 40] ^ 1),
                                  fix_crc=False),
        'entries of 64 bytes': header_edit(pristine, put('<I', 84, 64)),
        'entries of 192 bytes': header_edit(pristine, put('<I', 84, 192)),
        'no entries': header_edit(pristine, put('<I', 80, 0)),
        'a 2MiB array': header_edit(pristine, put('<I', 80, 16384)),
        'array LBA wrapping': header_edit(pristine, put('<Q', 72, (1 << 64) - 1)),
        'usable range backwards': header_edit(pristine, lambda data, at, lba: struct.pack_into(
            '<Q', data, at + 40, struct.unpack_from('<Q', data, at + 48)[0] + 1)),
        'array in the usable range': header_edit(pristine, lambda data, at, lba: struct.pack_into(
            '<Q', data, at + 40, 20) if lba == 1 else struct.pack_into('<Q', data, at + 48, lba - 1)),
        'array CRC': header_edit(pristine, lambda data, at, lba: data.__setitem__(
            struct.unpack_from('<Q', data, at + 72)[0] * 512 + 100, 0x5a), fix_crc=False),
        'medium too short': pristine[:5 * 512],
    }
    for name, data in variants.items():
        path = write(t, name.replace(' ', '-') + '.img', data)
        code, out, err = install(t, path)
        t.check(code != 0 and 'Installing to GPT' not in err and "start sector is less than 63" in err,
                f'{name}: not taken for a GPT: {err!r}')
    code, out, err = install(t, write(t, 'pristine.img', pristine))
    t.check(code == 0 and 'Installing to GPT' in err, f'the unspoilt disk installs to GPT: {err!r}')


@test(ports=['bios'], tags=['host'])
def host_gpt_not_iso(t):
    """A GPT disk is converted only if it is an ISO 9660 one, its primary
    volume descriptor among the first 256 [intent]: not where a terminator
    comes first, nor past 256 other descriptors, nor where the medium ends
    before the first one."""
    pristine = read(gpt_disk(t, 'base.img'))
    terminator = bytearray(pristine)
    terminator[32768:32768 + 7] = b'\xffCD001\x01'
    supplementary = bytearray(pristine)
    for i in range(256):
        supplementary[32768 + 2048 * i:32768 + 2048 * i + 7] = b'\x02CD001\x01'
    for name, data in (('terminator', terminator), ('supplementary', supplementary)):
        code, out, err = install(t, write(t, f'{name}.img', bytes(data)))
        t.check(code == 0 and 'ISOHYBRID' not in err and 'Installing to GPT' in err,
                f'{name}: installed to GPT, unconverted: {err!r}')
    # A GPT of 4 entries, so that it fits in 60 blocks.
    tiny = bytearray(60 * 512)
    struct.pack_into('<B3sB3sII', tiny, 446, 0, b'\0\2\0', 0xee, b'\xff\xff\xff', 1, 59)
    tiny[510:512] = b'\x55\xaa'
    tiny[512:512 + 92] = struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x10000, 92, 0, 0, 1, 59, 3, 57,
                                     bytes(16), 2, 4, 128, 0)
    gpt_rewrite(tiny, 1)
    code, out, err = install(t, write(t, 'tiny.img', bytes(tiny)))
    t.check(code != 0 and 'ISOHYBRID' not in err and 'no BIOS boot partition specified or' in err,
            f'a GPT medium ending before sector 64 of 512 bytes: {err!r}')


FNV_PRIME = 16777619


def fnv_step(h, b):
    return (h ^ b) * FNV_PRIME & 0xffffffff


def fnv1a32_zero_guid():
    """16 bytes whose FNV-1a hash is 0: a prefix of 12, then the last 4 met
    in the middle, as (h ^ b) * prime is 0 only for h == b."""
    inverse = pow(FNV_PRIME, -1, 1 << 32)
    back = {}
    for b16 in range(256):
        for b15 in range(256):
            back.setdefault((b16 * inverse & 0xffffffff) ^ b15, (b15, b16))
    for n in range(1 << 16):
        prefix = n.to_bytes(12, 'little')
        h = 2166136261
        for b in prefix:
            h = fnv_step(h, b)
        for b13 in range(256):
            h13 = fnv_step(h, b13)
            for b14 in range(256):
                hit = back.get(fnv_step(h13, b14))
                if hit is not None:
                    return prefix + bytes([b13, b14, *hit])
    raise RuntimeError('no FNV-1a preimage of 0 found')


@test(ports=['bios'], tags=['host', 'iso'])
def host_isohybrid_conversion_edges(t):
    """The corners of the conversion: an alternate header the end of the
    medium does not hold, one closer to the usable range than a reserve, a
    disk GUID whose hash is 0, which would be no disk ID, and partitions too
    far out for an MBR to describe [intent]."""
    pristine = read(hybrid_iso(t, 'base.iso'))
    alternate = struct.unpack_from('<Q', pristine, 512 + 32)[0]

    path = write(t, 'grown.iso', pristine + bytes(1 << 20))
    code, out, err = install(t, path)
    data = read(path)
    t.check(code == 0 and data[alternate * 512:alternate * 512 + 8] != b'EFI PART',
            f'grown, nothing at the end: the alternate the header names wiped: {err!r}')

    path = write(t, 'tight.iso', pristine)
    spoil(path, last_usable=alternate - 10, alternate=False)
    code, out, err = install(t, path)
    data = read(path)
    kept = slice((alternate - 10) * 512, (alternate - 9) * 512)
    t.check(code == 0 and data[alternate * 512:alternate * 512 + 8] != b'EFI PART' and data[kept] == pristine[kept],
            f'the usable range reaching into the reserve: wiped above it only: {err!r}')

    guid = fnv1a32_zero_guid()
    path = write(t, 'zero-hash.iso', pristine)
    spoil(path, disk_guid=guid, alternate=False)
    code, out, err = install(t, path)
    t.check(code == 0 and struct.unpack_from('<I', read(path), 0x1b8)[0] == 1,
            f'a disk GUID hashing to 0 gives disk ID 1: {err!r}')

    parts = gpt_entries(pristine)
    gap1 = len(parts) - 1
    # Sparse images, past 8GiB and past 2TiB, read only where the tool reads.
    cylinders = 1024 * 255 * 63
    blocks = cylinders + 2048
    path = write(t, 'chs.iso', pristine)
    with open(path, 'r+b') as f:
        f.truncate(blocks * 512)
        data = bytearray(f.read(64 * 512))
        gpt_rewrite(data, 1, entry(gap1, start=cylinders, end=cylinders + 40), last_usable=blocks - 34)
        f.seek(0)
        f.write(data)
        # The usable range now covers the old alternate, which would be refused.
        f.seek(alternate * 512)
        f.write(bytes(512))
    code, out, err = install(t, path)
    with open(path, 'rb') as f:
        mbr = f.read(512)
    os.remove(path)
    e = mbr[446 + 16 * gap1:462 + 16 * gap1]
    t.check(code == 0 and e[1:4] == e[5:8] == b'\xfe\xff\xff' and struct.unpack_from('<II', e, 8) == (cylinders, 41),
            f'a partition past what CHS can address: CHS saturated, LBA kept: {e.hex()}: {err!r}')

    blocks = (1 << 32) + (1 << 20)
    for name, start, end, message in (
            ('far-start', 1 << 32, (1 << 32) + 99, 'Starting LBA of partition'),
            ('long', parts[gap1][1], (1 << 32) + 99, 'Sector count of partition')):
        path = write(t, f'{name}.iso', pristine)
        with open(path, 'r+b') as f:
            f.truncate(blocks * 512)
        with open(path, 'r+b') as f:
            data = bytearray(f.read(64 * 512))
            gpt_rewrite(data, 1, entry(gap1, start=start, end=end), last_usable=blocks - 34)
            f.seek(0)
            f.write(data)
        code, out, err = install(t, path)
        t.check(code != 0 and f'{message} {gap1 + 1} is greater than UINT32_MAX, will not convert GPT.' in err,
                f'{name}: not converted: {err!r}')
        os.remove(path)


def gpt_header(my_lba, alternate_lba, first_usable, last_usable, array_lba, count, array_crc):
    h = bytearray(struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x10000, 92, 0, 0, my_lba, alternate_lba,
                              first_usable, last_usable, bytes(16), array_lba, count, 128, array_crc))
    struct.pack_into('<I', h, 16, zlib.crc32(bytes(h)))
    return bytes(h)


@test(ports=['bios'], tags=['host', 'hostile'])
def host_gpt_array_budget(t):
    """Looking for a GPT reads at most two entry arrays' worth, 2MiB: a
    primary and an alternate at the end, each with a 1MiB array that fails
    its CRC, use it up, so the alternate the primary names, though sound and
    holding a BIOS boot partition, is not read, and the disk is no GPT; with
    the one at the end gone, the same alternate is used [intent]."""
    blocks, count = 32768, 8192
    array = bytearray(count * 128)
    struct.pack_into('<16s16sQQ', array, 0, b'Hah!IdontNeedEFI', uuid.uuid4().bytes_le, 4096, 4096 + 2047)
    good = zlib.crc32(bytes(array))
    span = len(array) // 512
    first_usable, named = 2 + span, 30000
    last_usable = named - span - 1
    data = bytearray(blocks * 512)
    struct.pack_into('<B3sB3sII', data, 446, 0, b'\0\2\0', 0xee, b'\xff\xff\xff', 1, blocks - 1)
    data[510:512] = b'\x55\xaa'
    for lba, alternate, array_lba, crc in ((1, named, 2, good ^ 1), (blocks - 1, 1, blocks - 1 - span, good ^ 1),
                                           (named, 1, named - span, good)):
        data[array_lba * 512:(array_lba + span) * 512] = array
        data[lba * 512:lba * 512 + 92] = gpt_header(lba, alternate, first_usable, last_usable, array_lba, count, crc)
    code, out, err = install(t, write(t, 'three.img', bytes(data)))
    t.check(code != 0 and 'Installing to GPT' not in err, f'the third array not read: no GPT: {err!r}')
    data[(blocks - 1) * 512:blocks * 512] = bytes(512)
    code, out, err = install(t, write(t, 'two.img', bytes(data)))
    t.check(code == 0 and 'using the alternate header' in err and 'Autodetected partition 1' in err,
            f'with the one at the end gone, the alternate the primary names used: {err!r}')
