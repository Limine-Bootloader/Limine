import gzip
import hashlib

from harness.api import check
from harness.report import fnv1a
from cases._common import boot_kernel, check_report_clean, entry

# Memory above 4GiB as the Limine protocol's modules can use it; with the RAM
# below 4GiB cut to 128MiB, modules larger than that are loaded above it.
# The 32-bit ports reach it only through Limine's 64-bit copy routines.

LOW_RAM = 128 << 20
PERIOD = bytes((i * 7 + 3) % 251 for i in range(251))


def pattern(size):
    """size bytes of a 251-byte period: no power-of-2 stride repeats it, and
    gzip shrinks it to almost nothing."""
    return (PERIOD * (size // len(PERIOD) + 1))[:size]


@check(ports=['bios', 'uefi-ia32', 'uefi-x86-64'], tags=['limine-protocol', 'memory'], timeout=300)
def modules_above_4g(t):
    """Modules too large for the memory below 4GiB are put above it, whole:
    one read as it is, one decompressed as it is read, its buffer grown as it
    fills, and one compressed with a hash, checked where it lies before it is
    decompressed [intent]."""
    plain = pattern(140 << 20)
    unpacked = pattern(150 << 20)
    packed = gzip.compress(unpacked, 9, mtime=0)
    stored = pattern(130 << 20)
    hashed = gzip.compress(stored, 0, mtime=0)
    mods = [('boot():/boot/plain.bin', 'plain'), ('$boot():/boot/packed.gz', 'packed'),
            (f'$boot():/boot/hashed.gz#{hashlib.blake2b(hashed).hexdigest()}', 'hashed')]
    r, m, disk = boot_kernel(
        t, files={'/boot/plain.bin': plain, '/boot/packed.gz': packed, '/boot/hashed.gz': hashed}, modules=[],
        memory='1536M', timeout=240, disk_kwargs={'size_mb': 360},
        entry_text=entry('Test', path='boot():/boot/kernel.elf', cmdline='x', modules=mods),
        machine_kwargs={'machine_opts': [f'max-ram-below-4g={LOW_RAM >> 20}M']})
    check_report_clean(t, r)
    hhdm = int(r['hhdm.offset'], 16)
    for i, data in enumerate((plain, unpacked, stored)):
        prefix = f'module.{i:02}'
        phys = int(r[prefix + '.address'], 16) - hhdm
        t.check(phys >= 1 << 32, f'{prefix} above 4GiB: {phys:#x}')
        t.expect_eq(r.int(prefix + '.size'), len(data), f'{prefix} size')
        t.expect_eq(r[prefix + '.hash'], fnv1a(data), f'{prefix} contents')
