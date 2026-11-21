import glob
import os
import struct

from harness.api import check
from harness.elf import ElfFile
from harness.env import SRCDIR

# What the build makes of Limine's own sources, read from the objects it
# leaves: what a link only rejects where its layout happens to expose it.

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


@check(ports=['uefi-aarch64'], tags=['build'])
def build_aarch64_assembly_data_aligned(t):
    """Each global datum the AArch64 port's assembly defines is aligned to 8
    bytes, as C reaches it directly, with loads whose relocations need the
    address a multiple of the access size (R_AARCH64_LDST64_ABS_LO12_NC for
    64 bits), and the link fails wherever its layout leaves it otherwise."""
    sources = sorted(glob.glob(os.path.join(SRCDIR, 'common', '**', '*.asm_*aarch64'), recursive=True))
    t.require(sources, 'AArch64 assembly sources found')
    for source in sources:
        obj = os.path.splitext(os.path.relpath(source, SRCDIR))[0] + '.o'
        for name, offset, align in global_data(os.path.join(t.env.builddir, 'common-uefi-aarch64', obj)):
            t.check(offset % 8 == 0 and align >= 8,
                    f'{obj}: {name} at offset {offset:#x} in a section aligned to {align}')
