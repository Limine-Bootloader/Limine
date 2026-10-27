import os
import struct
import subprocess
import uuid

from harness import ptable
from harness.env import which

MiB = 1024 * 1024

ESP_TYPE = 'C12A7328-F81F-11D2-BA4B-00A0C93EC93B'
LINUX_TYPE = '0FC63DAF-8483-4772-8E79-3D69D8477DE4'
BIOS_BOOT_TYPE = '21686148-6449-6E6F-744E-656564454649'
BASIC_DATA_TYPE = 'EBD0A0A2-B9E5-4433-87C0-68B6B72699C7'
FREEBSD_BOOT_TYPE = '83BD6B9D-7F41-11DC-BE0B-001560B84F0F'

# A UTF-8 locale, for mtools to write names past ASCII as UTF-16 long names.
def _utf8_locale():
    """A UTF-8 locale the system has, for mtools to take long names in."""
    try:
        names = subprocess.run(['locale', '-a'], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                               universal_newlines=True).stdout.split()
    except OSError:
        names = []
    for name in ('C.UTF-8', 'C.utf8', 'en_US.UTF-8', 'en_US.utf8'):
        if name in names:
            return name
    return next((n for n in names if n.lower().endswith(('.utf-8', '.utf8'))), 'C.UTF-8')


_MTOOLS_ENV = dict(os.environ, MTOOLSRC='/dev/null', MTOOLS_SKIP_CHECK='1',
                   MTOOLS_NO_VFAT='0', TZ='UTC', LC_ALL=_utf8_locale())


def _run(argv, **kwargs):
    res = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kwargs)
    if res.returncode != 0:
        raise RuntimeError(f'command failed ({res.returncode}): {" ".join(argv)}\n'
                           f'{res.stdout.decode(errors="replace")}')
    return res.stdout


def _fat_type(path):
    """The FAT type of the volume path starts with, by its cluster count
    (Microsoft FAT Specification of 30 August 2005, 3.5)."""
    with open(path, 'rb') as f:
        bpb = f.read(40)
    sector, cluster, reserved, fats, roots, total16, _, fat16 = struct.unpack_from('<HBHBHHBH', bpb, 11)
    total32, fat32 = struct.unpack_from('<II', bpb, 32)
    data = (total16 or total32) - reserved - fats * (fat16 or fat32) - (roots * 32 + sector - 1) // sector
    clusters = data // cluster
    return 12 if clusters < 4085 else 16 if clusters < 65525 else 32


def _content(value):
    if isinstance(value, (bytes, bytearray)):
        return bytes(value)
    if isinstance(value, str):
        return value.encode()
    with open(value.path, 'rb') as f:
        return f.read()


class HostFile:
    """A host file's content, read when an image is built."""

    def __init__(self, path):
        self.path = path

    def __repr__(self):
        return f'HostFile({self.path!r})'


def file_bytes(value):
    return _content(value)


class Tree:
    """A set of files keyed by absolute path inside a filesystem."""

    def __init__(self, files=None):
        self.files = {}
        self.dirs = set()
        for path, value in (files or {}).items():
            self.add(path, value)

    def add(self, path, value):
        if not path.startswith('/'):
            path = '/' + path
        self.files[path] = value
        return self

    def mkdir(self, path):
        if not path.startswith('/'):
            path = '/' + path
        self.dirs.add(path.rstrip('/'))
        return self

    def all_dirs(self):
        dirs = set(self.dirs)
        for path in self.files:
            parent = os.path.dirname(path)
            while parent not in ('', '/'):
                dirs.add(parent)
                parent = os.path.dirname(parent)
        return sorted(dirs, key=lambda d: (d.count('/'), d))

    def materialise(self, root):
        for d in self.all_dirs():
            os.makedirs(root + d, exist_ok=True)
        for path, value in self.files.items():
            dest = root + path
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, 'wb') as f:
                f.write(_content(value))


class Fat:
    """A FAT filesystem to be written into an image at some offset."""

    def __init__(self, fat=32, label=None, volid=None, files=None, sector_size=512,
                 cluster_sectors=None, fats=None):
        self.fat = fat
        self.label = label
        self.volid = volid
        self.tree = files if isinstance(files, Tree) else Tree(files)
        self.sector_size = sector_size
        self.cluster_sectors = cluster_sectors
        self.fats = fats

    def add(self, path, value):
        self.tree.add(path, value)
        return self

    def write_into(self, image, offset, size, workdir):
        if offset % self.sector_size != 0:
            raise ValueError('FAT offset must be sector aligned')
        argv = ['-T', str(size // self.sector_size), '-h', '255', '-s', '63',
                '-S', str(self.sector_size.bit_length() - 8),
                '-N', f'{0x1234abcd if self.volid is None else self.volid:08x}']
        if self.fat == 32:
            argv.append('-F')
        if self.label is not None:
            argv += ['-v', self.label]
        if self.fats is not None:
            argv += ['-d', str(self.fats)]
        # mformat makes FAT12 or FAT16 by the cluster count its cluster size
        # gives, so cluster sizes are tried on a scratch image until one gives
        # the type asked for.
        probe = os.path.join(workdir, 'fatprobe.img')
        sizes = [self.cluster_sectors] if self.cluster_sectors else [None] + [1 << i for i in range(8)]
        for cluster in sizes:
            fit = argv if cluster is None else argv + ['-c', str(cluster)]
            with open(probe, 'wb') as f:
                f.truncate(size)
            _run([which('mformat'), '-i', probe, *fit, '::'], env=_MTOOLS_ENV)
            if _fat_type(probe) == self.fat:
                break
        else:
            raise ValueError(f'no FAT{self.fat} volume fits {size} bytes')
        os.unlink(probe)
        spec = f'{image}@@{offset}'
        _run([which('mformat'), '-i', spec, *fit, '::'], env=_MTOOLS_ENV)
        for d in self.tree.all_dirs():
            _run([which('mmd'), '-i', spec, '-D', 's', '::' + d], env=_MTOOLS_ENV)
        for path, value in self.tree.files.items():
            src = os.path.join(workdir, 'fatsrc.bin')
            with open(src, 'wb') as f:
                f.write(_content(value))
            _run([which('mcopy'), '-i', spec, '-D', 'o', '-m', src, '::' + path], env=_MTOOLS_ENV)
            os.unlink(src)


class Raw:
    """Arbitrary bytes placed at the start of a partition."""

    def __init__(self, data):
        self.data = data

    def write_into(self, image, offset, size, workdir):
        data = _content(self.data)
        if len(data) > size:
            raise ValueError('raw partition content larger than the partition')
        with open(image, 'r+b') as f:
            f.seek(offset)
            f.write(data)


class Part:
    def __init__(self, size_mb=None, fs=None, type=None, guid=None, name=None,
                 bootable=False, logical=False, start=None, size_sectors=None):
        self.size_mb = size_mb
        self.size_sectors = size_sectors
        self.fs = fs
        self.type = type
        self.guid = guid
        self.name = name
        self.bootable = bootable
        self.logical = logical
        self.start = start
        # Filled in by Disk.build().
        self.start_sector = None
        self.sectors = None
        self.number = None


class Disk:
    """A disk image: GPT, MBR, or a bare filesystem (scheme 'none')."""

    def __init__(self, scheme='gpt', size_mb=64, parts=None, fs=None, disk_guid=None,
                 mbr_id=None, sector_size=512):
        self.scheme = scheme
        self.size_mb = size_mb
        self.parts = parts or []
        self.fs = fs
        self.disk_guid = disk_guid
        self.mbr_id = mbr_id
        self.sector_size = sector_size

    def _logical_layout(self):
        """Explicit starts for an MBR with logical partitions, each with 1MiB
        before it for its EBR."""
        if self.scheme != 'mbr' or not any(p.logical for p in self.parts):
            return {}
        ss = self.sector_size
        align = MiB // ss
        starts = {}
        cursor = align
        for p in self.parts:
            sectors = p.size_sectors if p.size_sectors is not None else p.size_mb * MiB // ss
            if p.logical:
                cursor += align
            start = p.start if p.start is not None else cursor
            starts[id(p)] = start
            if str(p.type).lower() in ('5', '05', 'f', '0f', '85'):
                cursor = start
            else:
                cursor = -(-(start + sectors) // align) * align
        return starts

    def build(self, path, workdir=None):
        workdir = workdir or os.path.dirname(path)
        ss = self.sector_size
        with open(path, 'wb') as f:
            f.truncate(self.size_mb * MiB)
        if self.scheme == 'none':
            if self.fs is not None:
                self.fs.write_into(path, 0, self.size_mb * MiB, workdir)
            return path
        starts = self._logical_layout()
        specs = []
        for p in self.parts:
            sectors = p.size_sectors
            if sectors is None and p.size_mb is not None:
                sectors = p.size_mb * MiB // ss
            ptype = p.type
            if ptype is None:
                ptype = BASIC_DATA_TYPE if self.scheme == 'gpt' else '0c'
            specs.append({'start': p.start if p.start is not None else starts.get(id(p)), 'sectors': sectors,
                          'type': ptype, 'guid': p.guid, 'name': p.name, 'bootable': p.bootable,
                          'logical': p.logical})
        total = self.size_mb * MiB // ss
        with open(path, 'r+b') as f:
            if self.scheme == 'gpt':
                self.disk_guid, guids = ptable.write_gpt(f, total, ss, specs, self.disk_guid)
                numbers = range(1, len(specs) + 1)
            else:
                self.mbr_id, numbers = ptable.write_mbr(f, total, ss, specs, self.mbr_id)
                guids = [p.guid for p in self.parts]
        for p, spec, number, guid in zip(self.parts, specs, numbers, guids):
            p.start_sector, p.sectors, p.number, p.guid = spec['start'], spec['sectors'], number, guid
        for p in self.parts:
            if p.fs is not None:
                p.fs.write_into(path, p.start_sector * ss, p.sectors * ss, workdir)
        return path


def random_guid():
    return str(uuid.uuid4()).upper()


class Iso:
    """An ISO9660 image made with xorriso, optionally bootable on BIOS and/or
    UEFI."""

    def __init__(self, files=None, bios_boot=None, uefi_boot=None, rock_ridge=True, joliet=True,
                 volid='LIMINE_TEST', hybrid=True, extra_args=None):
        self.tree = files if isinstance(files, Tree) else Tree(files)
        self.bios_boot = bios_boot
        self.uefi_boot = uefi_boot
        self.rock_ridge = rock_ridge
        self.joliet = joliet
        self.volid = volid
        self.hybrid = hybrid
        self.extra_args = extra_args or []

    def add(self, path, value):
        self.tree.add(path, value)
        return self

    def build(self, path, workdir=None):
        workdir = workdir or os.path.dirname(path)
        root = os.path.join(workdir, 'isoroot')
        if os.path.exists(root):
            subprocess.run(['rm', '-rf', root], check=True)
        os.makedirs(root)
        self.tree.materialise(root)
        argv = [which('xorriso'), '-as', 'mkisofs', '-V', self.volid]
        # xorriso writes Rock Ridge unless told not to.
        argv += ['-R', '-r'] if self.rock_ridge else ['--norock']
        if self.joliet:
            argv += ['-J']
        if self.bios_boot is not None:
            argv += ['-b', self.bios_boot.lstrip('/'), '-no-emul-boot', '-boot-load-size', '4',
                     '-boot-info-table']
        if self.uefi_boot is not None:
            if self.hybrid:
                argv += ['-hfsplus', '-apm-block-size', '2048']
            argv += ['--efi-boot', self.uefi_boot.lstrip('/')]
            if self.hybrid:
                argv += ['-efi-boot-part', '--efi-boot-image']
        if self.hybrid:
            argv += ['--protective-msdos-label']
        argv += self.extra_args
        argv += [root, '-o', path]
        _run(argv)
        subprocess.run(['rm', '-rf', root], check=True)
        return path


def bios_install(env, image, partition=None, extra=None):
    argv = [env.bin('limine'), 'bios-install', '--quiet', image]
    if partition is not None:
        argv.append(str(partition))
    argv += extra or []
    return _run(argv)
