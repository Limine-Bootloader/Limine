import glob
import os
import struct

from harness.api import check
from harness.elf import ElfFile
from harness.env import SRCDIR

# What the build makes of Limine's own sources, read from the objects it
# leaves: what a link or a boot only shows where the layout exposes it.

SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
STB_GLOBAL = 1
SHN_LORESERVE = 0xff00


def global_data(path):
    """(name, offset, section alignment) of each global symbol an ELF64
    object defines in an allocated section that is not code."""
    with open(path, 'rb') as f:
        e = ElfFile(f.read())
    i = e.sections['.symtab']
    off, size, link = e.field(f'sh_offset{i}'), e.field(f'sh_size{i}'), e.field(f'sh_link{i}')
    strtab = e.field(f'sh_offset{link}')
    for at in range(off, off + size, 24):
        name, info, _, shndx, value, _ = struct.unpack_from('<IBBHQQ', e.data, at)
        if info >> 4 != STB_GLOBAL or not 0 < shndx < SHN_LORESERVE:
            continue
        flags = e.field(f'sh_flags{shndx}')
        if (flags & SHF_ALLOC) != 0 and (flags & SHF_EXECINSTR) == 0:
            yield (e.data[strtab + name:e.data.index(b'\0', strtab + name)].decode(), value,
                   e.field(f'sh_addralign{shndx}'))


@check(ports=['uefi-aarch64', 'uefi-riscv64', 'uefi-loongarch64'], tags=['build'])
def build_assembly_data_aligned(t):
    """Each global datum a port's assembly defines is aligned to 8 bytes, as C
    reaches it directly and loads it at its type's width: on AArch64 with
    relocations that need the address a multiple of that width
    (R_AARCH64_LDST64_ABS_LO12_NC for 64 bits), so that the link fails
    wherever its layout leaves it otherwise, and on RISC-V, built with
    -mstrict-align, and LoongArch with accesses the processor need not
    support unaligned."""
    arch = t.port.arch
    sources = sorted(glob.glob(os.path.join(SRCDIR, 'common', '**', f'*.asm_*{arch}'), recursive=True))
    t.require(sources, f'{arch} assembly sources found')
    builddir = os.path.dirname(t.env.limine_elf(t.port.name))
    for source in sources:
        obj = os.path.splitext(os.path.relpath(source, SRCDIR))[0] + '.o'
        for name, offset, align in global_data(os.path.join(builddir, obj)):
            t.check(offset % 8 == 0 and align >= 8,
                    f'{obj}: {name} at offset {offset:#x} in a section aligned to {align}')
