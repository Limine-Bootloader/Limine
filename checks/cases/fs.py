import gzip
import hashlib
import subprocess

from harness.api import test
from harness import media
from harness.media import Disk, Fat, HostFile, Iso, Part, Tree, BASIC_DATA_TYPE, ESP_TYPE
from harness.ports import ALL_PORTS, Drive
from harness.report import fnv1a, unescape
from cases._common import check_file, check_report_clean, entry, kernel_file, wait_panic

# Filesystems, partitions, paths and the decorations a path can carry. The
# expectations come from README.md (what is supported) and CONFIG.md (Paths,
# hashes and compression) unless marked [intent].

KERNEL = '/boot/kernel.elf'


def blake2b(data):
    return hashlib.blake2b(data).hexdigest()


def blake3(data, length=32):
    return subprocess.run(['b3sum', '--no-names', '-l', str(length)], input=data,
                          stdout=subprocess.PIPE, check=True).stdout.decode().strip()


def kernel_tree(t, tree=None):
    tree = tree or Tree()
    tree.add(KERNEL, HostFile(kernel_file(t, 'full')))
    return tree


def modules_config(modules, kernel_path='boot():' + KERNEL, cmdline='fs test', globals_='timeout: 0\n'):
    """One entry loading modules: (uri, string) pairs."""
    return globals_ + entry('Test', path=kernel_path, cmdline=cmdline, modules=modules)


def boot_report(t, m, timeout=60):
    report = t.kernel_report(m, timeout=timeout)
    check_report_clean(t, report)
    return report


@test(ports=ALL_PORTS, tags=['fs'])
def fs_fat_types(t):
    """Booting from FAT12, FAT16 and FAT32, each sized to be that type by its
    cluster count (Microsoft FAT Specification of 30 August 2005, 3.5)."""
    data = b'module on a FAT volume\n' * 300
    for fat, size, cluster in ((12, 8, 8), (16, 32, 4), (32, 64, 1)):
        tree = kernel_tree(t)
        tree.add('/boot/mod.bin', data)
        t.limine_files(modules_config([('boot():/boot/mod.bin', 'module')]), tree)
        disk = t.boot_disk(tree, fat=fat, size_mb=size, cluster_sectors=cluster)
        m = t.boot(disk)
        r = boot_report(t, m)
        t.expect_eq(unescape(r['cmdline']), 'fs test', f'FAT{fat}: booted')
        check_file(t, r, 'module.00', path='/boot/mod.bin', data=data, string='module', disk=disk)
        m.stop()


@test(ports=ALL_PORTS, tags=['fs'])
def fs_fat_names(t):
    """On FAT an 8.3 name matches whatever its case; a long name only as
    written."""
    short, long = b'short name contents', b'long name contents' * 20
    tree = kernel_tree(t)
    tree.add('/boot/short.bin', short)
    tree.add('/boot/A Long Module Name.bin', long)
    mods = [('boot():/BOOT/SHORT.BIN', 'short'), ('boot():/boot/A Long Module Name.bin', 'long')]
    t.limine_files(modules_config(mods, kernel_path='boot():/BOOT/Kernel.ELF'), tree)
    m = t.boot(t.boot_disk(tree))
    r = boot_report(t, m)
    t.expect_eq(r['module.00.hash'], fnv1a(short), '8.3 name found in another case')
    t.expect_eq(r['module.01.hash'], fnv1a(long), 'long name found as written')
    m.stop()
    tree = kernel_tree(t)
    tree.add('/boot/A Long Module Name.bin', long)
    t.limine_files(modules_config([('boot():/boot/a long module name.bin', None)]), tree)
    m = t.boot(t.boot_disk(tree))
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'limine: Failed to open module with path `boot():/boot/a long module name.bin`. '
                'Is the path correct?', 'a long name in another case not found')


def patch_long_name(path, field, units):
    """In the FAT image at path, the long name whose first five characters
    are field given the UTF-16 code units for its first len(units)
    characters instead: names mtools cannot write, such as those holding
    surrogates. The five sit together in a long name entry, as no others do."""
    with open(path, 'r+b') as f:
        image = bytearray(f.read())
        old = field.encode('utf-16-le')
        at = image.find(old)
        assert at >= 0 and image.find(old, at + 1) < 0, f'{field!r} in one long name entry'
        image[at:at + 2 * len(units)] = b''.join(u.to_bytes(2, 'little') for u in units)
        f.seek(0)
        f.write(image)


@test(ports=ALL_PORTS, tags=['fs', 'hostile'])
def fs_fat_unicode_names(t):
    """Long names are UTF-16 (Microsoft FAT Specification of 30 August 2005,
    section 7), matched against paths in UTF-8: names of two and three byte
    characters, and of one past the BMP, held as a surrogate pair. A
    surrogate without its other half reads as U+FFFD [intent]."""
    names = {'é-two-bytes.bin': None, '日本-three.bin': None, 'QQ-emoji.bin': ('QQ-em', [0xd83d, 0xde00]),
             'H-lone-high.bin': ('H-lon', [0xd800]), 'L-lone-low.bin': ('L-lon', [0xdc00])}
    read_as = {'QQ-emoji.bin': '😀-emoji.bin', 'H-lone-high.bin': '�-lone-high.bin',
               'L-lone-low.bin': '�-lone-low.bin'}
    tree = kernel_tree(t)
    mods = []
    for i, name in enumerate(names):
        tree.add('/' + name, f'module {i} named {name}\n'.encode() * 9)
        mods.append(('boot():/' + read_as.get(name, name), None))
    t.limine_files(modules_config(mods), tree)
    disk = t.boot_disk(tree)
    for patch in names.values():
        if patch is not None:
            patch_long_name(disk.path, *patch)
    m = t.boot(disk)
    r = boot_report(t, m)
    for i, name in enumerate(names):
        t.expect_eq(r[f'module.{i:02}.hash'], fnv1a(f'module {i} named {name}\n'.encode() * 9),
                    f'{read_as.get(name, name)!r} found')


@test(ports=ALL_PORTS, tags=['fs', 'hostile'])
def fs_fat32_active_fat(t):
    """[spec] With mirroring off, BPB_ExtFlags bit 7, only the FAT its bits 0-3
    number is in use (Microsoft FAT Specification of 30 August 2005, section
    3.3): a module is read through FAT 1 though FAT 0 is all zeroes. An active
    FAT past the FATs the volume has makes it no FAT volume [intent]."""
    import struct
    from cases.partitions import boot_with_data_disks, check_modules
    data = b'read through the second FAT\n' * 100
    disks = []
    for label, flags in (('ACTIVE1', 0x81), ('ACTIVE2', 0x82)):
        disk = Disk('gpt', 64, parts=[Part(size_mb=40, fs=Fat(32, label=label, files={'/m.bin': data},
                                                              cluster_sectors=1), type=BASIC_DATA_TYPE)])
        path = disk.build(t.path(f'{label}.img'), t.workdir)
        at = disk.parts[0].start_sector * 512
        with open(path, 'r+b') as f:
            f.seek(at)
            bpb = f.read(512)
            reserved, fats = struct.unpack_from('<H', bpb, 14)[0], bpb[16]
            fat_size = struct.unpack_from('<I', bpb, 36)[0]
            t.expect_eq(fats, 2, f'{label}: two FATs')
            f.seek(at + 40)
            f.write(struct.pack('<H', flags))
            f.seek(at + reserved * 512)
            f.write(bytes(fat_size * 512))
        disks.append((label, data, path))
    r = boot_with_data_disks(t, disks[:1])
    check_modules(t, r, disks[:1])
    tree = Tree()
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
    t.limine_files('timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x',
                                           modules=[('fslabel(ACTIVE2):/m.bin', 'two')]), tree)
    m = t.boot(drives=[Drive(t.boot_disk(tree).path, bootindex=1), Drive(disks[1][2], kind='virtio')])
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'limine: Failed to open module with path `fslabel(ACTIVE2):/m.bin`. Is the path correct?',
                'FAT 2 of 2 active: no FAT volume, so no such label')


def iso_boot(t, tree, **iso_kwargs):
    iso = t.iso(tree, **iso_kwargs)
    return t.boot(cdrom=iso)


@test(ports=ALL_PORTS, tags=['fs'])
def fs_iso9660_rock_ridge(t):
    """Rock Ridge names keep their case and length and match as written."""
    data = b'rock ridge module\n' * 40
    tree = kernel_tree(t)
    tree.add('/Mixed/Case-Name.Module', data)
    tree.add('/boot/limine.conf', modules_config([('boot():/Mixed/Case-Name.Module', 'rr')]))
    m = iso_boot(t, tree)
    r = boot_report(t, m)
    t.expect_eq(r['module.00.hash'], fnv1a(data), 'Rock Ridge name found as written')
    t.expect_eq(r.int('module.00.media_type'), 1, '[spec] media type optical (PROTOCOL.md)')
    m.stop()
    tree = kernel_tree(t)
    tree.add('/Mixed/Case-Name.Module', data)
    tree.add('/boot/limine.conf', modules_config([('boot():/mixed/case-name.module', None)]))
    m = iso_boot(t, tree)
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'limine: Failed to open module with path `boot():/mixed/case-name.module`. '
                'Is the path correct?', 'a Rock Ridge name in another case not found')


# ISO 9660 level 3 names hold up to 31 characters, and relaxed ones keep the
# '-' of limine-bios.sys; xorriso uppercases them.
PLAIN_ISO = ['-iso-level', '3', '-relaxed-filenames']


@test(ports=ALL_PORTS, tags=['fs'])
def fs_iso9660_rr_identifiers(t):
    """[intent] Rock Ridge is recognised by the ER entry of RRIP 1.12 writers
    too, IEEE_1282 or IEEE_P1282, and not only RRIP_1991A: on such volumes,
    data disks here, names keep their case. A path component longer than a
    Rock Ridge name can be stops the boot."""
    import os
    from harness.env import which
    data = b'rock ridge module, newer ER\n' * 30
    root = t.path('isoroot-rr')
    os.makedirs(root)
    with open(os.path.join(root, 'MixedCase.Module'), 'wb') as f:
        f.write(data)
    new = t.path('ieee1282.iso')
    subprocess.run([which('xorriso'), '-outdev', new, '-volid', 'RRNEW', '-compliance', 'new_rr',
                    '-map', root, '/'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    draft = t.path('ieeep1282.iso')
    Iso(Tree({'/MixedCase.Module': data}), volid='RRDRAFT', hybrid=False).build(draft, t.workdir)
    with open(draft, 'r+b') as f:
        image = f.read()
        t.expect_eq(image.count(b'RRIP_1991A'), 1, 'one ER entry to rename')
        f.seek(image.index(b'RRIP_1991A'))
        f.write(b'IEEE_P1282')
    with open(new, 'rb') as f:
        t.check(b'IEEE_1282' in f.read(), 'xorriso wrote an IEEE_1282 ER entry')
    tree = Tree()
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
    mods = [(f'fslabel({label}):/MixedCase.Module', label) for label in ('RRNEW', 'RRDRAFT')]
    t.limine_files('timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x', modules=mods), tree)
    drives = [Drive(t.boot_disk(tree).path, bootindex=1), Drive(new, kind='virtio'), Drive(draft, kind='virtio')]
    m = t.boot(drives=drives)
    r = t.kernel_report(m)
    for i, label in enumerate(('RRNEW', 'RRDRAFT')):
        t.expect_eq(r[f'module.{i:02}.hash'], fnv1a(data), f'{label}: the name found as written')
    m.stop()
    tree = Tree()
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
    t.limine_files('timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x',
                                           modules=[('fslabel(RRNEW):/' + 'n' * 300, 'long')]), tree)
    m = t.boot(drives=[Drive(t.boot_disk(tree).path, bootindex=1), Drive(new, kind='virtio')])
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'iso9660: Path component exceeds maximum length', 'a 300 character component')


@test(ports=ALL_PORTS, tags=['fs'])
def fs_iso9660_plain(t):
    """Without Rock Ridge, ISO 9660 names (uppercase) match whatever the case,
    the config file included; Joliet is not read."""
    data = b'plain iso module\n' * 40
    tree = kernel_tree(t)
    tree.add('/boot/mod.bin', data)
    tree.add('/boot/limine.conf', modules_config([('boot():/boot/mod.bin', 'plain')]))
    m = iso_boot(t, tree, rock_ridge=False, joliet=False, extra_args=PLAIN_ISO)
    r = boot_report(t, m)
    t.expect_eq(r['module.00.hash'], fnv1a(data), 'ISO 9660 name matched in lowercase')
    m.stop()
    long = 'A-Name-Too-Long-For-ISO-9660-Level-3.module'
    tree = kernel_tree(t)
    tree.add('/boot/' + long, data)
    tree.add('/boot/limine.conf', modules_config([(f'boot():/boot/{long}', None)]))
    m = iso_boot(t, tree, rock_ridge=False, joliet=True, extra_args=PLAIN_ISO)
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, f'limine: Failed to open module with path `boot():/boot/{long}`. Is the path correct?',
                'a name only Joliet holds in full is not found')


@test(ports=ALL_PORTS, tags=['fs'])
def fs_hashes(t):
    """A path may end in #<hash>: 128 hex digits for BLAKE2b, 64 for BLAKE3 and
    256 for extended-output BLAKE3; a matching file loads. The BLAKE3 files
    span several of its 1 KiB chunks, which it hashes as a tree."""
    data = [b'blake2b checked\n' * 10, b'blake3 checked\n' * 300, b'long blake3 checked\n' * 400]
    tree = kernel_tree(t)
    for i, d in enumerate(data):
        tree.add(f'/boot/h{i}.bin', d)
    kernel = open(kernel_file(t, 'full'), 'rb').read()
    mods = [(f'boot():/boot/h0.bin#{blake2b(data[0])}', 'b2'),
            (f'boot():/boot/h1.bin#{blake3(data[1])}', 'b3'),
            (f'boot():/boot/h2.bin#{blake3(data[2], 128)}', 'b3 long')]
    t.limine_files(modules_config(mods, kernel_path=f'boot():{KERNEL}#{blake2b(kernel)}'), tree)
    m = t.boot(t.boot_disk(tree))
    r = boot_report(t, m)
    for i, d in enumerate(data):
        t.expect_eq(r[f'module.{i:02}.hash'], fnv1a(d), f'module {i} loaded')
        t.expect_eq(unescape(r[f'module.{i:02}.path']), f'/boot/h{i}.bin', f'[intent] module {i} path without its hash')


@test(ports=ALL_PORTS, tags=['fs'])
def fs_hash_mismatch(t):
    """A hash that does not match panics unless hash_mismatch_panic is no; a
    hash of no supported length is refused."""
    data = b'the real contents\n'
    wrong2 = blake2b(b'other contents')
    wrong3 = blake3(b'other contents')
    cases = [(f'boot():/boot/m.bin#{wrong2}', 'BLAKE2b hash for URI `boot():/boot/m.bin` does not match!'),
             (f'boot():/boot/m.bin#{wrong3}', 'BLAKE3 hash for URI `boot():/boot/m.bin` does not match!'),
             ('boot():/boot/m.bin#' + 'ab' * 16,
              '[intent]Hash must be 64 BLAKE3, 128 BLAKE2b, or 256 BLAKE3 characters long')]
    for uri, message in cases:
        tree = kernel_tree(t)
        tree.add('/boot/m.bin', data)
        t.limine_files(modules_config([(uri, None)]), tree)
        m = t.boot(t.boot_disk(tree))
        msg, _ = wait_panic(t, m)
        t.expect_eq(msg, message.replace('[intent]', ''), message)
        m.stop()


@test(ports=ALL_PORTS, tags=['fs'])
def fs_hash_mismatch_no_panic(t):
    """With hash_mismatch_panic: no a mismatch asks; Y boots on, anything else
    returns to the menu. So for a gzip file, whose hash is of its compressed
    bytes."""
    data = b'the real contents\n' * 50
    other = blake2b(b'other contents')
    files = {'/boot/m.bin': data, '/boot/m.gz': gzip.compress(data, mtime=0)}
    for uri in (f'boot():/boot/m.bin#{other}', f'$boot():/boot/m.gz#{other}'):
        for key, booted in (('y', True), ('n', False)):
            tree = kernel_tree(t)
            for path, contents in files.items():
                tree.add(path, contents)
            t.limine_files(modules_config([(uri, 'kept')], globals_='timeout: 0\nhash_mismatch_panic: no\n'),
                           tree)
            m = t.boot(t.boot_disk(tree))
            t.wait_text(m, 'does not match!')
            t.wait_text(m, 'Press Y to continue')
            m.press(key)
            if booted:
                r = boot_report(t, m)
                t.expect_eq(r['module.00.hash'], fnv1a(data), f'{uri[:20]}: Y: booted with the file as it is')
            else:
                t.wait_text(m, 'ARROWS')
                t.check('B LIMINE-TEST' not in m.stream(t.kernel_channel).text(),
                        f'{uri[:20]}: another key: back to the menu')
            m.stop()


@test(ports=ALL_PORTS, tags=['fs'])
def fs_gzip(t):
    """$ before the resource inflates a gzip file; its hash is that of the
    compressed bytes, anything after the gzip data included."""
    data = bytes(range(256)) * 300 + b'end of the inflated module'
    packed = gzip.compress(data, mtime=0) + b'trailing bytes the hash covers'
    tree = kernel_tree(t)
    tree.add('/boot/m.gz', packed)
    mods = [('$boot():/boot/m.gz', 'plain'), (f'$boot():/boot/m.gz#{blake2b(packed)}', 'hashed')]
    t.limine_files(modules_config(mods), tree)
    m = t.boot(t.boot_disk(tree))
    r = boot_report(t, m)
    for i in range(2):
        t.expect_eq(r.int(f'module.{i:02}.size'), len(data), f'module {i} inflated size')
        t.expect_eq(r[f'module.{i:02}.hash'], fnv1a(data), f'module {i} inflated contents')
    m.stop()
    tree = kernel_tree(t)
    tree.add('/boot/m.gz', packed)
    t.limine_files(modules_config([(f'$boot():/boot/m.gz#{blake2b(data)}', None)]), tree)
    m = t.boot(t.boot_disk(tree))
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'BLAKE2b hash for URI `$boot():/boot/m.gz` does not match!',
                'a hash of the inflated data does not match')


def uri_media(t):
    """A boot disk with a second partition (and on MBR an extended one with a
    logical partition), an unpartitioned FAT disk, and a data CD."""
    m2, m5, whole, cd = b'second partition\n' * 30, b'logical partition\n' * 31, b'whole disk\n' * 32, b'cd\n' * 33
    boot = kernel_tree(t)
    if t.port.is_uefi:
        parts = [Part(size_mb=40, fs=Fat(16, label='LIMINE', files=boot), type=ESP_TYPE),
                 Part(size_mb=20, fs=Fat(16, label='SECONDPART', files={'/m2.bin': m2}), type=BASIC_DATA_TYPE)]
        disk = Disk('gpt', 64, parts=parts)
    else:
        parts = [Part(size_mb=30, fs=Fat(16, label='LIMINE', files=boot), type='0e', bootable=True),
                 Part(size_mb=12, fs=Fat(16, label='SECONDPART', files={'/m2.bin': m2}), type='0e'),
                 Part(size_mb=14, type='5'),
                 Part(size_mb=12, fs=Fat(16, label='LOGICAL', files={'/m5.bin': m5}), type='0e', logical=True)]
        disk = Disk('mbr', 64, parts=parts, mbr_id=0x5eed1e55)
    files = {'boot': (boot, disk), 'm2': m2, 'm5': m5, 'whole': whole, 'cd': cd}
    second = Disk('none', 16, fs=Fat(16, label='WHOLEDISK', files={'/whole.bin': whole}))
    iso = Iso({'/cd.bin': cd}, volid='MODULECD', hybrid=False)
    return files, disk, second, iso


@test(ports=ALL_PORTS, tags=['fs'])
def fs_resources(t):
    """boot(), hdd() with and without a partition, odd(), guid()/uuid() and
    fslabel() reach the volumes they name; partitions 5 and up are logical."""
    files, disk, second, iso = uri_media(t)
    mods = [('boot(2):/m2.bin', 'boot(2)'), ('hdd(1:2):/m2.bin', 'hdd(1:2)'), ('hdd(2:):/whole.bin', 'hdd(2:)'),
            ('odd(1:):/cd.bin', 'odd(1:)'), ('fslabel(secondpart):/m2.bin', 'fslabel'),
            ('fslabel(ModuleCD):/cd.bin', 'fslabel cd')]
    if not t.port.is_uefi:
        mods.append(('boot(5):/m5.bin', 'boot(5)'))
    boot_tree = files['boot'][0]
    t.limine_files(None, boot_tree)
    disk.build(t.path('a.img'), t.workdir)
    if t.port.is_uefi:
        guid = disk.parts[1].guid
        mods += [(f'guid({guid}):/m2.bin', 'guid'), (f'uuid({guid.lower()}):/m2.bin', 'uuid')]
    # The config goes in last, once the partition GUID is known.
    boot_tree.add('/boot/limine.conf', modules_config(mods))
    disk.parts[0].fs.write_into(t.path('a.img'), disk.parts[0].start_sector * 512, disk.parts[0].sectors * 512,
                                t.workdir)
    if not t.port.is_uefi:
        media.bios_install(t.env, t.path('a.img'))
    second.build(t.path('b.img'), t.workdir)
    iso.build(t.path('c.iso'), t.workdir)
    m = t.machine(drives=[Drive(t.path('a.img'), bootindex=0), Drive(t.path('b.img')),
                          Drive(t.path('c.iso'), media='cdrom')])
    r = boot_report(t, m)
    expect = {'boot(2)': ('/m2.bin', files['m2'], 2), 'hdd(1:2)': ('/m2.bin', files['m2'], 2),
              'hdd(2:)': ('/whole.bin', files['whole'], 0), 'odd(1:)': ('/cd.bin', files['cd'], 0),
              'fslabel': ('/m2.bin', files['m2'], 2), 'fslabel cd': ('/cd.bin', files['cd'], 0),
              'boot(5)': ('/m5.bin', files['m5'], 5), 'guid': ('/m2.bin', files['m2'], 2),
              'uuid': ('/m2.bin', files['m2'], 2)}
    for i, (uri, string) in enumerate(mods):
        path, data, partition = expect[string]
        on_cd = string in ('odd(1:)', 'fslabel cd')
        on_boot_disk = partition != 0
        check_file(t, r, f'module.{i:02}', path=path, data=data, string=string, partition=partition,
                   disk=disk if on_boot_disk else None, media_type=1 if on_cd else 0)


@test(ports=['uefi-ia32', 'uefi-x86-64'], tags=['fs'])
def fs_unconnected_disk(t):
    """[user] A volume on a disk that shares the boot disk's AHCI controller,
    which OVMF's fast boot leaves unconnected, is found all the same by
    hdd(), guid() and fslabel()."""
    second = Disk('gpt', 32, parts=[Part(fs=Fat(16, label='SECONDDISK', files=kernel_tree(t)),
                                         type=BASIC_DATA_TYPE)])
    second.build(t.path('second.img'), t.workdir)
    for path in ('hdd(2:1):' + KERNEL, f'guid({second.parts[0].guid}):' + KERNEL, 'fslabel(SECONDDISK):' + KERNEL):
        disk = t.boot_disk(t.limine_files('timeout: 0\n' + entry('Test', path=path, cmdline=path)))
        m = t.machine(drives=[Drive(disk.path, bootindex=0), Drive(t.path('second.img'))])
        t.expect_eq(unescape(t.kernel_report(m)['cmdline']), path, f'booted from {path}')
        m.stop()
