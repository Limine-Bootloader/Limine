"""PE32+ images for the Limine protocol's PE loader. from_elf() remakes an
ELF check executable: a section for each loadable segment, at the same place
relative to the lowest one, which becomes the image base, and, for a position
independent executable, base relocations from its R_*_RELATIVE dynamic
relocations (PE Format of 07/14/2025, "The .reloc Section (Image Only)").
synthetic() makes a small image to spoil. fields tells where each header
field lives, for hostile variants."""

import struct

# e_machine: (PE Machine, R_*_RELATIVE)
MACHINES = {62: (0x8664, 8), 183: (0xaa64, 1027), 243: (0x5064, 3), 258: (0x6264, 3)}
PE_MACHINES = {'x86_64': 0x8664, 'aarch64': 0xaa64, 'riscv64': 0x5064, 'loongarch64': 0x6264}
PT_LOAD, PT_DYNAMIC = 1, 2
PF_X, PF_W = 1, 2
DT_NULL, DT_RELA, DT_RELASZ, DT_RELAENT = 0, 7, 8, 9
SCN_CODE, SCN_IDATA = 0x20, 0x40
SCN_EXECUTE, SCN_READ, SCN_WRITE = 0x20000000, 0x40000000, 0x80000000
FILE_RELOCS_STRIPPED, FILE_EXECUTABLE_IMAGE, FILE_LARGE_ADDRESS_AWARE = 0x1, 0x2, 0x20
REL_BASED_DIR64 = 10
PAGE, FILE_ALIGN = 0x1000, 0x200
NT = 0x40
OPT = NT + 4 + 20
SECTIONS = OPT + 240
HIGHER_HALF = 0xffffffff80000000


def align(v, a):
    return (v + a - 1) // a * a


class PeImage:
    def __init__(self, data, fields):
        self.data = bytes(data)
        self.fields = fields

    def patched(self, **values):
        """A copy with the given fields set."""
        data = bytearray(self.data)
        for name, value in values.items():
            off, fmt = self.fields[name]
            struct.pack_into('<' + fmt, data, off, value)
        return bytes(data)


def reloc_blocks(entries):
    """Base relocation blocks for (rva, type) entries."""
    pages = {}
    for rva, kind in entries:
        pages.setdefault(rva & ~(PAGE - 1), []).append(kind << 12 | rva & (PAGE - 1))
    out = b''
    for page, words in sorted(pages.items()):
        if len(words) % 2:
            words.append(0)
        out += struct.pack(f'<II{len(words)}H', page, 8 + 2 * len(words), *words)
    return out


def build(machine, image_base, entry_rva, sections, relocatable, reloc_dir=(0, 0)):
    """An image of sections, each [name, rva, virtual size, data,
    characteristics]."""
    headers = align(SECTIONS + 40 * len(sections), FILE_ALIGN)
    size_of_image = align(max(s[1] + s[2] for s in sections), PAGE)
    out = bytearray(headers)
    fields = {'e_magic': (0, 'H'), 'e_lfanew': (0x3c, 'I')}
    struct.pack_into('<H', out, 0, 0x5a4d)
    struct.pack_into('<I', out, 0x3c, NT)
    chars = FILE_EXECUTABLE_IMAGE | FILE_LARGE_ADDRESS_AWARE | (0 if relocatable else FILE_RELOCS_STRIPPED)
    struct.pack_into('<IHHIIIHH', out, NT, 0x4550, machine, len(sections), 0, 0, 0, 240, chars)
    for name, off, fmt in (('Signature', 0, 'I'), ('Machine', 4, 'H'), ('NumberOfSections', 6, 'H'),
                           ('SizeOfOptionalHeader', 20, 'H'), ('Characteristics', 22, 'H')):
        fields[name] = (NT + off, fmt)
    struct.pack_into('<HBBIIIII', out, OPT, 0x20b, 0, 0, 0, 0, 0, entry_rva, 0)
    struct.pack_into('<QII', out, OPT + 24, image_base, PAGE, FILE_ALIGN)
    struct.pack_into('<II', out, OPT + 56, size_of_image, headers)
    struct.pack_into('<H', out, OPT + 68, 10)
    struct.pack_into('<I', out, OPT + 108, 16)
    struct.pack_into('<II', out, OPT + 112 + 5 * 8, *reloc_dir)
    for name, off, fmt in (('Magic', 0, 'H'), ('AddressOfEntryPoint', 16, 'I'), ('ImageBase', 24, 'Q'),
                           ('SectionAlignment', 32, 'I'), ('SizeOfImage', 56, 'I'), ('SizeOfHeaders', 60, 'I'),
                           ('NumberOfRvaAndSizes', 108, 'I'), ('ImportRva', 120, 'I'), ('ImportSize', 124, 'I'),
                           ('RelocRva', 152, 'I'), ('RelocSize', 156, 'I')):
        fields[name] = (OPT + off, fmt)
    for i, (name, rva, vsize, data, schars) in enumerate(sections):
        raw = len(out)
        out += data + bytes(align(len(data), FILE_ALIGN) - len(data))
        struct.pack_into('<8sIIIIIIHHI', out, SECTIONS + 40 * i, name, vsize, rva,
                         align(len(data), FILE_ALIGN), raw, 0, 0, 0, 0, schars)
        for field, off, fmt in (('VirtualSize', 8, 'I'), ('VirtualAddress', 12, 'I'), ('SizeOfRawData', 16, 'I'),
                                ('PointerToRawData', 20, 'I'), ('SectionCharacteristics', 36, 'I')):
            fields[f'{field}{i}'] = (SECTIONS + 40 * i + off, fmt)
    return PeImage(out, fields)


def from_elf(elf):
    machine, etype = struct.unpack_from('<H', elf, 18)[0], struct.unpack_from('<H', elf, 16)[0]
    entry, phoff = struct.unpack_from('<QQ', elf, 24)
    phentsize, phnum = struct.unpack_from('<HH', elf, 54)
    pe_machine, r_relative = MACHINES[machine]
    phdrs = [struct.unpack_from('<IIQQQQQQ', elf, phoff + i * phentsize) for i in range(phnum)]
    loads = sorted((p for p in phdrs if p[0] == PT_LOAD), key=lambda p: p[3])
    base = loads[0][3] & ~(PAGE - 1)

    def file_offset(vaddr):
        for _, _, off, va, _, filesz, _, _ in loads:
            if va <= vaddr < va + filesz:
                return off + vaddr - va
        raise ValueError(f'{vaddr:#x} has no file data')

    relocs = []
    if etype == 3:
        dyn = next(p for p in phdrs if p[0] == PT_DYNAMIC)
        tags = {}
        for i in range(dyn[5] // 16):
            tag, val = struct.unpack_from('<qQ', elf, dyn[2] + i * 16)
            if tag == DT_NULL:
                break
            tags[tag] = val
        start = file_offset(tags[DT_RELA]) if DT_RELA in tags else 0
        for i in range(tags.get(DT_RELASZ, 0) // tags.get(DT_RELAENT, 24)):
            r_offset, r_info, r_addend = struct.unpack_from('<QQq', elf, start + i * 24)
            if r_info & 0xffffffff != r_relative:
                raise ValueError(f'relocation type {r_info & 0xffffffff} at {r_offset:#x}')
            relocs.append((r_offset, r_addend))

    sections = []
    for _, flags, off, va, _, filesz, memsz, _ in loads:
        data = bytearray(elf[off:off + filesz])
        # What the location holds with the image at its base; the loader adds
        # the slide.
        for r_offset, addend in relocs:
            if va <= r_offset < va + filesz:
                struct.pack_into('<Q', data, r_offset - va, addend)
        chars = SCN_READ | (SCN_EXECUTE | SCN_CODE if flags & PF_X else SCN_IDATA)
        if flags & PF_W:
            chars |= SCN_WRITE
        name = b'.text' if flags & PF_X else b'.data' if flags & PF_W else b'.rdata'
        sections.append([name, va - base, memsz, bytes(data), chars])
    reloc_dir = (0, 0)
    if relocs:
        blocks = reloc_blocks((r_offset - base, REL_BASED_DIR64) for r_offset, _ in relocs)
        reloc_dir = (align(max(s[1] + s[2] for s in sections), PAGE), len(blocks))
        sections.append([b'.reloc', reloc_dir[0], len(blocks), blocks, SCN_READ | SCN_IDATA])
    return build(pe_machine, base, entry - base, sections, etype == 3, reloc_dir)


def synthetic(arch, data=b'', relocatable=False, image_base=HIGHER_HALF, reloc_dir=(0, 0)):
    """A two section image never meant to run: .text at 0x1000 and .data,
    holding data, at 0x2000."""
    sections = [[b'.text', 0x1000, 0x100, bytes(0x100), SCN_READ | SCN_EXECUTE | SCN_CODE],
                [b'.data', 0x2000, max(len(data), 0x100), data or bytes(0x100), SCN_READ | SCN_WRITE | SCN_IDATA]]
    return build(PE_MACHINES[arch], image_base, 0x1000, sections, relocatable, reloc_dir)
