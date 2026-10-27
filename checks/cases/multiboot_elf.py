from harness.api import check
from harness.elf import ElfFile
from harness.ports import X86_PORTS
from cases._common import boot_entry, entry, panics_in_turn, read
from cases.multiboot import mb1_kernel, mb2_kernel

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
    msgs = panics_in_turn(t, m, len(cases))
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
