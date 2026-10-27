import gzip
import struct

from harness.api import check
from harness.elf import ElfFile
from harness.ports import ALL_PORTS
from harness.report import fnv1a
from cases._common import (boot_entry, boot_kernel, check_file, check_report_clean, entry, kernel_file,
                           panics_in_turn, read)
from cases.limine_protocol import INTERNAL_COMPRESSED, INTERNAL_REQUIRED, internal_module_files

# Executables whose requests point where they must not, built by patching the
# check executables; the messages are Limine's own [intent].

OUTSIDE = 0x1000
COMMON_MAGIC = (0xc7b1dd30df4c8b88, 0x0a82e883a194f07b)
# struct limine_module_request: internal_module_count and internal_modules.
IM_COUNT, IM_ARRAY = 48, 56
# struct limine_internal_module: path and string.
IM_PATH, IM_STRING = 0, 8


def q(value):
    return struct.pack('<Q', value)


def internal_module_cases(t):
    e = ElfFile(read(kernel_file(t, 'im1')))
    sym = e.symbols()
    request = e.offset_of(sym['module_request'][0])
    array_va = sym['internal_modules'][0]
    array = e.offset_of(array_va)
    first_va = sym['im_required'][0]
    first = e.offset_of(first_va)
    # A path longer than any the bootloader builds, written over the start
    # of the code, which never runs.
    text = e.section('.text')[1] + 0x100
    long_path = e.with_bytes(text, b'a' * 1100 + b'\0')
    long_path_va = e.phdrs[e.loads()[0]]['p_vaddr'] + text - e.phdrs[e.loads()[0]]['p_offset']
    return [
        ('more internal modules than can be counted', e.with_bytes(request + IM_COUNT, q(1 << 61)),
         'limine: Too many internal modules'),
        ('internal module array outside', e.with_bytes(request + IM_ARRAY, q(OUTSIDE)),
         'limine: Internal module array is outside the executable'),
        ('internal module array running off the end', e.with_bytes(request + IM_COUNT, q(1 << 40)),
         'limine: Internal module array is outside the executable'),
        ('internal module array misaligned', e.with_bytes(request + IM_ARRAY, q(array_va + 4)),
         'limine: Internal module array is outside the executable'),
        ('internal module outside', e.with_bytes(array, q(OUTSIDE)),
         'limine: Internal module is outside the executable'),
        ('internal module misaligned', e.with_bytes(array, q(first_va + 4)),
         'limine: Internal module is outside the executable'),
        ('internal module path outside', e.with_bytes(first + IM_PATH, q(OUTSIDE)),
         'limine: Internal module path or string is outside the executable'),
        ('internal module string outside', e.with_bytes(first + IM_STRING, q(OUTSIDE)),
         'limine: Internal module path or string is outside the executable'),
        ('internal module path too long',
         ElfFile(long_path).with_bytes(first + IM_PATH, q(long_path_va)), 'limine: Internal module path too long'),
    ]


def too_many_requests(t):
    """129 requests of IDs no feature has, over the code of an executable
    without request markers, so that the whole image is scanned."""
    e = ElfFile(read(kernel_file(t, 'nomarkers')))
    text = e.section('.text')[1] + 0x100 & ~7
    fake = b''.join(struct.pack('<QQQQ', *COMMON_MAGIC, 0x5445535452455100 + i, i) for i in range(129))
    return e.with_bytes(text, fake)


@check(ports=ALL_PORTS, tags=['limine-protocol', 'hostile'], timeout=600)
def limine_hostile_requests(t):
    """An entry without an executable, internal modules the request places
    outside the executable or describes impossibly, and more requests than
    the bootloader keeps: each a panic, and the menu comes back."""
    cases = [('no executable path', None, 'limine: Executable path not specified')]
    cases += internal_module_cases(t)
    cases.append(('129 requests', too_many_requests(t), 'limine: Maximum requests exceeded'))
    files, entries = {}, ''
    for i, (_, image, _) in enumerate(cases):
        if image is None:
            entries += entry(f'L{i:02}')
            continue
        files[f'/boot/l{i:02}.elf'] = image
        entries += entry(f'L{i:02}', path=f'boot():/boot/l{i:02}.elf')
    m, _ = boot_entry(t, 'timeout: no\n' + entries, files=files, kernel_variant=None)
    msgs = panics_in_turn(t, m, len(cases))
    for (what, _, want), got in zip(cases, msgs):
        t.check(got == want, f'{what}: {got!r}, want {want!r}')


@check(ports=ALL_PORTS, tags=['limine-protocol', 'fs'])
def limine_compressed_executable(t):
    """A gzip compressed executable ($ before its path, CONFIG.md) boots, the
    executable file response holding it inflated; internal modules resolve
    against its directory with the $ left off, so an uncompressed one is not
    inflated."""
    data = read(kernel_file(t, 'im1'))
    files = internal_module_files()
    files['/boot/kernel.elf.gz'] = gzip.compress(data, mtime=0)
    r, m, disk = boot_kernel(t, 'im1', files=files, modules=[],
                             entry_text=entry('Test', path='$boot():/boot/kernel.elf.gz', cmdline='x'))
    check_report_clean(t, r)
    t.expect_eq(r.int('executable_file.size'), len(data), 'executable file response: inflated size')
    t.expect_eq(r['executable_file.hash'], fnv1a(data), 'executable file response: inflated contents')
    check_file(t, r, 'module.00', path='/boot/im/required.bin', data=INTERNAL_REQUIRED, string='internal required',
               disk=disk)
    check_file(t, r, 'module.03', path='/boot/im/compressed.bin.gz', data=INTERNAL_COMPRESSED,
               string='internal compressed', disk=disk)
