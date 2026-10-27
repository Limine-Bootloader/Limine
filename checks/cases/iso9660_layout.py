import struct

from harness.api import check
from harness.media import HostFile, Iso, Tree
from harness.ports import ALL_PORTS, Drive
from harness.report import fnv1a
from cases._common import check_report_clean, entry, kernel_file

# ISO 9660 layouts xorriso does not make for small files, made by editing
# what it does (ECMA-119 6th edition): a file recorded in several extents,
# each record but the last flagged multi-extent (10.1.7), and a directory
# spanning more than one sector, each record wholly within one (7.8.1.2).

SECTOR = 2048
MULTI_EXTENT = 0x80
PLAIN = ['-iso-level', '3', '-relaxed-filenames']


def records(sector):
    """(offset, record) of each directory record in a sector."""
    out, i = [], 0
    while i < SECTOR and sector[i] != 0:
        out.append((i, sector[i:i + sector[i]]))
        i += sector[i]
    return out


def with_extent(record, lba, size, flags):
    r = bytearray(record)
    struct.pack_into('<I', r, 2, lba)
    struct.pack_into('>I', r, 6, lba)
    struct.pack_into('<I', r, 10, size)
    struct.pack_into('>I', r, 14, size)
    r[25] = flags
    return bytes(r)


def split_into_extents(image, name, order):
    """The root directory's record of name, recorded instead as one extent
    per sector of its data, taken in the given order of those sectors.
    Returns the data the file then holds."""
    root_lba, root_size = struct.unpack_from('<I', image, 16 * SECTOR + 156 + 2)[0], \
        struct.unpack_from('<I', image, 16 * SECTOR + 156 + 10)[0]
    t_sector = root_lba * SECTOR
    sector = bytes(image[t_sector:t_sector + SECTOR])
    recs = records(sector)
    at, rec = next((o, r) for o, r in recs if r[33:33 + r[32]] == name)
    lba, size = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
    assert size == len(order) * SECTOR
    parts = [with_extent(rec, lba + s, SECTOR, rec[25] | (MULTI_EXTENT if i + 1 < len(order) else 0))
             for i, s in enumerate(order)]
    rebuilt = sector[:at] + b''.join(parts) + sector[at + len(rec):]
    used = sum(len(r) for _, r in recs) + len(rec) * (len(order) - 1)
    assert used <= SECTOR and root_size >= SECTOR
    image[t_sector:t_sector + SECTOR] = rebuilt[:SECTOR].ljust(SECTOR, b'\0')
    data = image[lba * SECTOR:(lba + len(order)) * SECTOR]
    return b''.join(data[s * SECTOR:(s + 1) * SECTOR] for s in order)


@check(ports=ALL_PORTS, tags=['fs', 'iso9660'])
def iso9660_extents_and_long_directories(t):
    """A file in three extents, out of the order of its sectors on the disc,
    reads as its records give them; a file whose record lies in a
    directory's second sector, past the zeros ending the first, is found."""
    multi = b''.join(bytes([i]) * SECTOR for i in (0x11, 0x22, 0x33))
    last = b'the last file of a long directory\n' * 30
    files = {'/MULTI.BIN': multi, '/DIR/ZZZ_LAST.BIN': last}
    for i in range(80):
        files[f'/DIR/FILE_NUMBER_{i:02}.BIN'] = b'x'
    iso = Iso(files, volid='LAYOUTCD', hybrid=False, rock_ridge=False, joliet=False, extra_args=PLAIN)
    path = t.path('layout.iso')
    iso.build(path, t.workdir)
    with open(path, 'r+b') as f:
        image = bytearray(f.read())
        expected = split_into_extents(image, b'MULTI.BIN;1', (2, 0, 1))
        f.seek(0)
        f.write(image)
    tree = Tree()
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
    mods = [('odd(1:):/multi.bin', 'multi'), ('odd(1:):/dir/zzz_last.bin', 'last')]
    t.limine_files('timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x', modules=mods), tree)
    boot = t.boot_disk(tree)
    m = t.boot(drives=[Drive(boot.path, bootindex=1), Drive(path, media='cdrom')])
    r = t.kernel_report(m)
    check_report_clean(t, r)
    t.expect_eq((r.int('module.00.size'), r['module.00.hash']), (len(expected), fnv1a(expected)),
                '[spec] the extents read in the order recorded')
    t.expect_eq(r['module.01.hash'], fnv1a(last), "[spec] the record in the directory's second sector found")
