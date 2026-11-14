import time

from harness.api import check
from harness.elf import PT_LOAD, ElfFile
from harness.ports import X86_PORTS
from cases._common import boot_entry, entry, panics_in_turn, read, select_entry, wait_panic
from cases.multiboot import (boot_mb1, boot_mb2, mb1_kernel, mb1_report, mb2_kernel, mb2_report)

# Spoilt ELF kernels for Multiboot 1 and 2, each failing one check of
# Limine's loader and no other first, in an ELF32 file and in the ELF64
# container Limine also takes; the messages are Limine's own [intent].

RISCV = 0xf3


def hostile_images(e, proto):
    bits32 = e.field('ei_class') == 1
    loads = e.loads()
    first, last = loads[0], loads[-1]
    size = len(e.data)
    cases = [
        ('big-endian', e.patched(ei_data=2), 'elf: Not a Little-endian ELF file.'),
        ('unaligned program headers', e.patched(e_phoff=e.field('e_phoff') + 2),
         'elf: Header table is not naturally aligned in the file'),
        ('unaligned section headers', e.patched(e_shoff=e.field('e_shoff') + 2),
         'elf: Header table is not naturally aligned in the file'),
        ('relocatable object', e.patched(e_type=1), 'elf: ELF file not of type ET_EXEC nor ET_DYN'),
        ('short program headers', e.patched(e_phentsize=16),
         f'elf: phdr_size < sizeof(struct elf{32 if bits32 else 64}_phdr)'),
        ('program headers past the end', e.patched(e_phoff=size & ~7),
         'elf: Program header table extends beyond file bounds'),
        ('no loadable segments', e.patched(**{f'p_type{i}': 0 for i in loads}), 'elf: No loadable segments'),
        ('extent beyond 4GiB', e.patched(**{f'p_paddr{first}': 0, f'p_paddr{last}': 0xfffff000}),
         'elf: Image extent exceeds the load limit'),
        ('more in the file than in memory', e.patched(**{f'p_filesz{first}': e.phdrs[first]['p_memsz'] + 1}),
         'elf: p_filesz > p_memsz'),
        ('segment data past the end', e.patched(**{f'p_offset{first}': size}),
         'elf: p_offset + p_filesz exceeds file size'),
        ('overlapping segments', e.patched(**{f'p_paddr{last}': e.phdrs[first]['p_paddr']}),
         'elf: Overlapping loadable segments'),
        ('overlapping segments larger together than memory',
         e.patched(**{f'p_paddr{i}': e.phdrs[first]['p_paddr'] for i in loads},
                   **{f'p_memsz{i}': 0xa000000 for i in loads}),
         'elf: Overlapping loadable segments'),
        ('entry outside every segment', e.patched(e_entry=0x1000), f'{proto}: Entry point is outside the executable'),
        ('short section headers', e.patched(e_shentsize=20 if bits32 else 32),
         f'{proto}: ELF section entry size too small'),
    ]
    if bits32:
        cases.append(('another machine', e.patched(e_machine=RISCV), 'elf: Not an IA-32 ELF file.'))
    else:
        cases += [
            ('another machine', e.patched(e_machine=RISCV), 'elf: Not an x86-64 ELF file.'),
            ('segment end overflow', e.patched(**{f'p_paddr{last}': 0xfffffffffffff000}),
             'elf: p_paddr + p_memsz overflow'),
            ('segment offset overflow', e.patched(**{f'p_offset{first}': (1 << 64) - 4}),
             'elf: p_offset + p_filesz overflow'),
            ('above 4GiB', e.patched(**{f'p_paddr{i}': e.phdrs[i]['p_paddr'] + (1 << 32) for i in loads}),
             f'{proto}: Executable does not fit under 4GiB'),
        ]
    return cases


def run_hostile(t, cases, proto):
    files, entries = {}, ''
    for i, (_, image, _) in enumerate(cases):
        files[f'/boot/k{i:02}.elf'] = image
        entries += entry(f'K{i:02}', protocol=proto, path=f'boot():/boot/k{i:02}.elf')
    m, _ = boot_entry(t, 'timeout: no\n' + entries, files=files, kernel_variant=None)
    msgs = panics_in_turn(t, m, [f'K{i:02}' for i in range(len(cases))])
    for (what, _, want), got in zip(cases, msgs):
        t.check(got == want, f'{what}: {got!r}, want {want!r}')


@check(ports=X86_PORTS, tags=['multiboot1', 'elf', 'hostile'], timeout=900)
def mb1_elf_hostile(t):
    """Spoilt Multiboot 1 ELF kernels end in the panic for what is wrong with
    them, and the menu comes back."""
    cases = []
    for name in ('kernel.elf', 'kernel64.elf'):
        e = ElfFile(read(mb1_kernel(t, 'elf', name)))
        cases += [(f'{name}: {what}', image, msg) for what, image, msg in hostile_images(e, 'multiboot1')]
    run_hostile(t, cases, 'multiboot1')


@check(ports=X86_PORTS, tags=['multiboot2', 'elf', 'hostile'], timeout=900)
def mb2_elf_hostile(t):
    """The same for Multiboot 2."""
    e = ElfFile(read(mb2_kernel(t)))
    run_hostile(t, hostile_images(e, 'multiboot2'), 'multiboot2')


def distant_header(path):
    e = ElfFile(read(path))
    first = e.loads()[0]
    return e.patched(**{f'p_paddr{first}': 0x10000000})


@check(ports=X86_PORTS, tags=['multiboot1', 'elf'])
def mb1_disjoint_elf(t):
    """Load a kernel whose header segment is far from its other segments."""
    m = boot_mb1(t, distant_header(mb1_kernel(t)))
    mb1_report(t, m)


@check(ports=X86_PORTS, tags=['multiboot2', 'elf'])
def mb2_disjoint_elf(t):
    """Load a kernel whose header segment is far from its other segments."""
    m = boot_mb2(t, distant_header(mb2_kernel(t)))
    mb2_report(t, m)


def target_over_staging(path):
    """The kernel moved to 256MiB, behind a first segment of 128MiB at 260MiB,
    over where the staging buffers of the others would otherwise go."""
    e = ElfFile(read(path))
    moved = [dict(p, p_paddr=p['p_paddr'] + 0xfd00000) if p['p_type'] == PT_LOAD else p for p in e.phdrs]
    big = {'p_type': PT_LOAD, 'p_offset': 0, 'p_vaddr': 0x10400000, 'p_paddr': 0x10400000, 'p_filesz': 0,
           'p_memsz': 0x8000000, 'p_flags': 4, 'p_align': 0x1000}
    return e.with_phdrs([big] + moved)


@check(ports=X86_PORTS, tags=['multiboot2', 'elf'])
def mb2_target_over_staging(t):
    """Load a kernel, position independent, with a segment over where the
    loader would otherwise stage the others [intent]."""
    m = boot_mb2(t, target_over_staging(mb2_kernel(t)))
    mb2_report(t, m)


def many_segments(path):
    """600 more segments, of a page each with a page between them, from
    128MiB."""
    e = ElfFile(read(path))
    page = {'p_type': PT_LOAD, 'p_offset': 0, 'p_filesz': 0, 'p_memsz': 0x1000, 'p_flags': 4, 'p_align': 0x1000}
    extra = [dict(page, p_vaddr=0x8000000 + i * 0x2000, p_paddr=0x8000000 + i * 0x2000) for i in range(600)]
    return e.with_phdrs(e.phdrs + extra)


@check(ports=X86_PORTS, tags=['multiboot1', 'elf'])
def mb1_many_segments(t):
    """Load a kernel with too many segments apart to reserve one by one
    [intent]."""
    m = boot_mb1(t, many_segments(mb1_kernel(t)))
    mb1_report(t, m)


@check(ports=X86_PORTS, tags=['multiboot2', 'elf'])
def mb2_many_segments(t):
    """The same for Multiboot 2."""
    m = boot_mb2(t, many_segments(mb2_kernel(t)))
    mb2_report(t, m)


def overlapping_segments(path, descending):
    """65535 segments of two pages, a page apart so that each overlaps the
    next, in ascending or descending order."""
    e = ElfFile(read(path))
    order = range(65534, -1, -1) if descending else range(65535)
    return e.with_phdrs([{'p_type': PT_LOAD, 'p_offset': 0, 'p_vaddr': 0x1000000 + i * 0x1000,
                          'p_paddr': 0x1000000 + i * 0x1000, 'p_filesz': 0, 'p_memsz': 0x2000, 'p_flags': 4,
                          'p_align': 0x1000} for i in order])


@check(ports=X86_PORTS, tags=['multiboot2', 'elf', 'hostile'])
def mb2_elf_segments_sorted(t):
    """65535 overlapping segments are refused about as soon in descending
    order as in ascending, which an insertion sort takes quadratic time
    over [intent]."""
    files = {'/boot/a.elf': overlapping_segments(mb2_kernel(t), False),
             '/boot/d.elf': overlapping_segments(mb2_kernel(t), True)}
    config = ('timeout: no\n' + entry('A', protocol='multiboot2', path='boot():/boot/a.elf')
              + entry('D', protocol='multiboot2', path='boot():/boot/d.elf'))
    m, _ = boot_entry(t, config, files=files, kernel_variant=None)
    taken = []
    for i, name in enumerate(('A', 'D')):
        t.wait_text(m, 'ARROWS', 60)
        select_entry(t, m, i, name)
        start = time.monotonic()
        m.press('ret')
        msg, _ = wait_panic(t, m)
        taken.append(time.monotonic() - start)
        t.expect_eq(msg, 'elf: Overlapping loadable segments', f'{name}: the panic')
        m.press('ret')
    t.check(taken[1] < 4 * taken[0] + 2 * t.port.timeout_scale,
            f'descending in {taken[1]:.1f}s, ascending in {taken[0]:.1f}s')
