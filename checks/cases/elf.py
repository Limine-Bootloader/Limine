from harness.api import payload_arch, check
from harness import elf
from harness.elf import (DT_JMPREL, DT_NEEDED, DT_PLTREL, DT_PLTRELSZ, DT_REL, DT_RELA, DT_RELAENT, DT_RELASZ,
                         DT_RELR, DT_RELRENT, DT_RELRSZ, DT_STRTAB, DT_SYMENT, DT_SYMTAB, ET_DYN, HIGHER_HALF,
                         JMPREL, RELA, RELR, STRTAB, SYMTAB)
from harness.ports import ALL_PORTS
from cases._common import boot_entry, entry, panics_in_turn

# Spoilt ELF executables for the Limine protocol, each made to fail one check
# of Limine's loader and no other first; the messages are Limine's own
# [intent]. Where a file breaks the ELF gABI 4.3 itself, that is noted.

WRONG_MACHINE = {'x86_64': 'elf: Not an x86-64 ELF file.', 'aarch64': 'elf: Not an aarch64 ELF file.',
                 'riscv64': 'elf: Not a riscv64 ELF file.', 'loongarch64': 'elf: Not a loongarch64 ELF file.'}
UNKNOWN = 'limine: Unknown kernel executable format'
OUTSIDE = 0x9000000
STB_GLOBAL, STB_WEAK = 1, 2


def dyn(arch, tags, **kwargs):
    """A relocatable image based at 0 with the given dynamic tags."""
    return elf.synthetic(arch, etype=ET_DYN, base=0, dynamic=tags, **kwargs).data


def table(name, offset):
    return (name, ('data', offset))


def hostile_images(arch, bits32):
    none, relative, jump_slot, abs64 = elf.RELOCS[elf.MACHINES[arch]]
    other = 183 if arch == 'x86_64' else 62
    s = elf.synthetic(arch)
    size = len(s.data)
    d = elf.synthetic(arch, etype=ET_DYN, base=0, dynamic=[])
    target = elf.data_va(0, 0x900)
    rela_tags = [table(DT_RELA, RELA), (DT_RELASZ, 24), (DT_RELAENT, 24)]
    sym_tags = [table(DT_SYMTAB, SYMTAB), (DT_SYMENT, 24), table(DT_STRTAB, STRTAB)]
    undefined = [(0, 0, 0, 0), (1, STB_GLOBAL << 4, 0, 0)]
    cases = [
        ('big-endian', s.patched(ei_data=2), 'elf: Not a Little-endian ELF file.'),
        ('another machine', s.patched(e_machine=other), WRONG_MACHINE[arch]),
        # x86-64 and aarch64 exist only as ELFCLASS64, so such a file is no
        # ELF Limine knows; RISC-V and LoongArch have 32-bit forms.
        ('32-bit class', s.patched(ei_class=1),
         UNKNOWN if arch in ('x86_64', 'aarch64') else WRONG_MACHINE[arch]),
        ('unaligned program headers', s.patched(e_phoff=68), 'elf: Header table is not naturally aligned in the file'),
        ('unaligned header size', s.patched(e_phentsize=60), 'elf: Header table is not naturally aligned in the file'),
        ('relocatable object', s.patched(e_type=1), 'elf: ELF file not of type ET_EXEC nor ET_DYN'),
        ('short program header', s.patched(e_phentsize=48), 'elf: phdr_size < sizeof(struct elf64_phdr)'),
        ('program header offset overflow', s.patched(e_phoff=0xfffffffffffffff8),
         'elf: Program header table size overflow'),
        ('program headers past the end', s.patched(e_phoff=size),
         'elf: Program header table extends beyond file bounds'),
        ('ET_DYN without PT_DYNAMIC', elf.synthetic(arch, etype=ET_DYN, base=0).data,
         'elf: ELF file type is ET_DYN, but PT_DYNAMIC segment missing'),
        ('empty PT_DYNAMIC', d.patched(p_filesz2=0), 'elf: ELF file type is ET_DYN, but PT_DYNAMIC segment has 0 size'),
        ('alignment not a power of 2', s.patched(p_align0=0x3000), 'elf: p_align is not a power of 2'),
        ('no loadable segments', s.patched(p_type0=0, p_type1=0), 'elf: Executable has no loadable segments'),
        ('fixed lower half', elf.synthetic(arch, base=0).data, 'elf: Lower half PHDRs are not allowed'),
        ('lower then higher half', d.patched(p_vaddr1=HIGHER_HALF + 0x2000),
         'elf: Mix of lower and higher half PHDRs in relocatable kernel'),
        ('higher then lower half', elf.synthetic(arch, etype=ET_DYN, dynamic=[]).patched(p_vaddr1=0x2000),
         'elf: Mix of lower and higher half PHDRs in relocatable kernel'),
        ('segment end overflow', s.patched(p_vaddr0=0xfffffffffffff800, p_memsz0=0x1000),
         'elf: p_vaddr + p_memsz overflow in PHDR 0'),
        ('overlapping segments', s.patched(p_vaddr1=HIGHER_HALF + 0x1080),
         'elf: Attempted to load ELF file with overlapping or out of order PHDRs (1)'),
        ('permissions sharing a page', s.patched(p_vaddr1=HIGHER_HALF + 0x1100),
         'elf: Attempted to load ELF file with PHDRs with different permissions sharing the same memory page.'),
        ('segment page past the top', s.patched(p_vaddr0=0xfffffffffffff000, p_memsz0=0xfff),
         'elf: PHDR alignment overflow'),
        ('more in the file than in memory', s.patched(p_filesz0=0x200), 'elf: p_filesz > p_memsz'),
        ('segment offset overflow', s.patched(p_offset0=0xffffffffffffff00), 'elf: p_offset + p_filesz overflow'),
        ('segment data past the end', s.patched(p_offset0=size - 0x80), 'elf: p_offset + p_filesz exceeds file size'),
        ('unaligned dynamic segment', d.patched(p_offset2=0x2004), 'elf: Failed to apply relocations'),
        ('a needed library', dyn(arch, [(DT_NEEDED, 1)]), 'elf: ELF file attempts to load a dynamically linked library'),
        ('short symbol entries', dyn(arch, [(DT_SYMENT, 8)]), 'elf: symtab_ent < sizeof(struct elf64_sym)'),
        ('short relocation entries', dyn(arch, [(DT_RELAENT, 8)]), 'elf: rela_ent < sizeof(struct elf64_rela)'),
        ('relocations outside', dyn(arch, [(DT_RELA, OUTSIDE), (DT_RELASZ, 24), (DT_RELAENT, 24)]),
         'elf: RELA vaddr translation failed or out of bounds'),
        ('relocation size alone', dyn(arch, [(DT_RELASZ, 24), (DT_RELAENT, 24)]), 'elf: DT_RELASZ without DT_RELA'),
        ('packed relocations outside', dyn(arch, [(DT_RELR, OUTSIDE), (DT_RELRSZ, 8), (DT_RELRENT, 8)]),
         'elf: RELR vaddr translation failed or out of bounds'),
        ('packed relocation size alone', dyn(arch, [(DT_RELRSZ, 8)]), 'elf: DT_RELRSZ without DT_RELR'),
        ('symbols outside', dyn(arch, [(DT_SYMTAB, OUTSIDE)]), 'elf: SYMTAB vaddr translation failed or out of bounds'),
        ('strings outside', dyn(arch, [(DT_STRTAB, OUTSIDE)]), 'elf: STRTAB vaddr translation failed or out of bounds'),
        ('PLT relocations outside', dyn(arch, [(DT_JMPREL, OUTSIDE), (DT_PLTRELSZ, 24), (DT_PLTREL, DT_RELA)]),
         'elf: JMPREL vaddr translation failed or out of bounds'),
        ('PLT relocation size alone', dyn(arch, [(DT_PLTRELSZ, 24)]), 'elf: DT_PLTRELSZ without DT_JMPREL'),
        ('unaligned symbol entries', dyn(arch, [table(DT_SYMTAB, SYMTAB), (DT_SYMENT, 28)]),
         'elf: Dynamic table is not naturally aligned in the file'),
        ('packed relocation entries not 8', dyn(arch, [table(DT_RELR, RELR), (DT_RELRSZ, 8), (DT_RELRENT, 4)]),
         'elf: relr_ent != 8'),
        ('relocations without an entry size', dyn(arch, [table(DT_RELA, RELA), (DT_RELASZ, 24)]),
         'elf: rela_size != 0 but rela_ent == 0'),
        ('relocation size not whole entries', dyn(arch, [table(DT_RELA, RELA), (DT_RELASZ, 32), (DT_RELAENT, 24)]),
         'elf: rela_size not a multiple of rela_ent'),
        ('PLT relocations as REL', dyn(arch, [table(DT_JMPREL, JMPREL), (DT_PLTRELSZ, 24), (DT_PLTREL, DT_REL)]),
         'elf: dt_pltrel != DT_RELA'),
        ('PLT relocation size not whole entries',
         dyn(arch, [table(DT_JMPREL, JMPREL), (DT_PLTRELSZ, 32), (DT_PLTREL, DT_RELA)]),
         'elf: dt_pltrelsz not a multiple of sizeof(struct elf64_rela)'),
        ('unknown relocation', dyn(arch, rela_tags, rela=[(target, 0x7ff, 0, 0)]), 'elf: Unknown relocation type: 0x7ff'),
    ]
    for kind, what in ((jump_slot, 'slot'), (abs64, 'absolute')):
        cases += [
            (f'{what} relocation without symbols', dyn(arch, rela_tags, rela=[(target, kind, 1, 0)]),
             'elf: Relocation requires symbol table but none present'),
            (f'{what} relocation symbol past the table', dyn(arch, rela_tags + sym_tags, rela=[(target, kind, 1000, 0)]),
             'elf: Symbol index 1000 out of bounds'),
            (f'{what} relocation symbol offset overflow',
             dyn(arch, rela_tags + [table(DT_SYMTAB, SYMTAB), (DT_SYMENT, 1 << 62)], rela=[(target, kind, 4, 0)]),
             'elf: Symbol offset overflow'),
            (f'{what} relocation to an undefined symbol',
             dyn(arch, rela_tags + sym_tags, rela=[(target, kind, 1, 0)], symbols=undefined, strtab=b'\0missing\0'),
             'elf: Unresolved symbol "missing"'),
            (f'{what} relocation symbol name past the strings',
             dyn(arch, rela_tags + sym_tags, rela=[(target, kind, 1, 0)],
                 symbols=[(0, 0, 0, 0), (0xfffff, STB_GLOBAL << 4, 0, 0)]),
             'elf: Symbol name offset out of bounds'),
            (f'{what} relocation to an undefined symbol without strings',
             dyn(arch, rela_tags + [table(DT_SYMTAB, SYMTAB), (DT_SYMENT, 24)], rela=[(target, kind, 1, 0)],
                 symbols=undefined),
             'elf: Strtab vaddr translation failed'),
        ]
    if bits32:
        cases.append(('alignment beyond 32 bits', s.patched(p_align0=1 << 33),
                      'elf: p_align is too large for a 32-bit port'))
    return cases


@check(ports=ALL_PORTS, tags=['limine-protocol', 'elf', 'hostile'], timeout=1200)
def elf_hostile(t):
    """Spoilt executables end in the panic for what is wrong with them, never
    a crash, and the menu comes back."""
    cases = hostile_images(payload_arch(t.port), t.port.name in ('bios', 'uefi-ia32'))
    files, entries = {}, ''
    for i, (_, image, _) in enumerate(cases):
        files[f'/boot/h{i:02}.elf'] = image
        entries += entry(f'H{i:02}', path=f'boot():/boot/h{i:02}.elf')
    m, _ = boot_entry(t, 'timeout: no\n' + entries, files=files)
    msgs = panics_in_turn(t, m, len(cases))
    for (what, _, want), got in zip(cases, msgs):
        t.check(got == want, f'{what}: {got!r}, want {want!r}')
