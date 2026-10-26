"""Small ELF images never meant to run, for spoiling: a text segment at
0x1000 and a data segment at 0x2000 above a base, the data segment holding
whatever dynamic table, relocations, symbols and strings are asked for, at
fixed places. fields tells where each header field lives."""

import struct

ET_REL, ET_EXEC, ET_DYN = 1, 2, 3
PT_LOAD, PT_DYNAMIC = 1, 2
PF_X, PF_W, PF_R = 1, 2, 4
HIGHER_HALF = 0xffffffff80000000
MACHINES = {'x86_64': 62, 'aarch64': 183, 'riscv64': 243, 'loongarch64': 258}
# R_*_NONE, R_*_RELATIVE, R_*_JUMP_SLOT, R_*_64 per machine.
RELOCS = {62: (0, 8, 7, 1), 183: (0, 1027, 1026, 257), 243: (0, 3, 5, 2), 258: (0, 3, 5, 2)}
DT_NULL, DT_NEEDED, DT_PLTRELSZ, DT_STRTAB, DT_SYMTAB = 0, 1, 2, 5, 6
DT_RELA, DT_RELASZ, DT_RELAENT, DT_SYMENT, DT_REL, DT_PLTREL, DT_JMPREL = 7, 8, 9, 11, 17, 20, 23
DT_RELRSZ, DT_RELR, DT_RELRENT = 35, 36, 37
# Where the tables go, from the start of the data segment.
DYNAMIC, RELA, SYMTAB, STRTAB, RELR, JMPREL = 0, 0x200, 0x400, 0x600, 0x700, 0x800
TEXT_OFF, DATA_OFF = 0x1000, 0x2000


class ElfImage:
    def __init__(self, data, fields):
        self.data = bytes(data)
        self.fields = fields

    def patched(self, **values):
        data = bytearray(self.data)
        for name, value in values.items():
            off, fmt = self.fields[name]
            struct.pack_into('<' + fmt, data, off, value)
        return bytes(data)


def synthetic(arch, etype=ET_EXEC, base=HIGHER_HALF, dynamic=None, rela=(), relr=(), jmprel=(),
              symbols=(), strtab=b'', extra_phdrs=()):
    """dynamic: (tag, value) pairs, values naming tables by the offsets above
    as ('data', offset); rela and jmprel: (r_offset, type, symbol, addend);
    relr: words; symbols: (st_name, st_info, st_shndx, st_value)."""
    machine = MACHINES[arch]
    text_va, data_va = base + 0x1000, base + 0x2000
    seg = bytearray(0x1000)

    def value(v):
        return data_va + v[1] if isinstance(v, tuple) else v

    for i, (tag, val) in enumerate(list(dynamic or ()) + ([(DT_NULL, 0)] if dynamic is not None else [])):
        struct.pack_into('<qQ', seg, DYNAMIC + 16 * i, tag, value(val))
    for table, entries in ((RELA, rela), (JMPREL, jmprel)):
        for i, (r_offset, kind, sym, addend) in enumerate(entries):
            struct.pack_into('<QIIq', seg, table + 24 * i, r_offset, kind, sym, addend)
    for i, word in enumerate(relr):
        struct.pack_into('<Q', seg, RELR + 8 * i, word)
    for i, (name, info, shndx, val) in enumerate(symbols):
        struct.pack_into('<IBBHQQ', seg, SYMTAB + 24 * i, name, info, 0, shndx, val, 0)
    seg[STRTAB:STRTAB + len(strtab)] = strtab

    phdrs = [(PT_LOAD, PF_R | PF_X, TEXT_OFF, text_va, 0x100, 0x100, 0x1000),
             (PT_LOAD, PF_R | PF_W, DATA_OFF, data_va, 0x1000, 0x1000, 0x1000)]
    if dynamic is not None:
        phdrs.append((PT_DYNAMIC, PF_R | PF_W, DATA_OFF + DYNAMIC, data_va + DYNAMIC, 16 * (len(dynamic) + 1),
                      16 * (len(dynamic) + 1), 8))
    phdrs += list(extra_phdrs)
    out = bytearray(DATA_OFF + 0x1000)
    out[DATA_OFF:] = seg
    struct.pack_into('<4sBBBB8xHHIQQQIHHHHHH', out, 0, b'\x7fELF', 2, 1, 1, 0, etype, machine, 1, text_va,
                     64, 0, 0, 64, 56, len(phdrs), 64, 0, 0)
    fields = {'ei_class': (4, 'B'), 'ei_data': (5, 'B'), 'e_type': (16, 'H'), 'e_machine': (18, 'H'),
              'e_entry': (24, 'Q'), 'e_phoff': (32, 'Q'), 'e_shoff': (40, 'Q'), 'e_phentsize': (54, 'H'),
              'e_phnum': (56, 'H'), 'e_shentsize': (58, 'H'), 'e_shnum': (60, 'H'), 'e_shstrndx': (62, 'H')}
    for i, phdr in enumerate(phdrs):
        struct.pack_into('<IIQQQQQQ', out, 64 + 56 * i, phdr[0], phdr[1], phdr[2], phdr[3], phdr[3],
                         phdr[4], phdr[5], phdr[6])
        for name, off, fmt in (('p_type', 0, 'I'), ('p_flags', 4, 'I'), ('p_offset', 8, 'Q'), ('p_vaddr', 16, 'Q'),
                               ('p_filesz', 32, 'Q'), ('p_memsz', 40, 'Q'), ('p_align', 48, 'Q')):
            fields[f'{name}{i}'] = (64 + 56 * i + off, fmt)
    return ElfImage(out, fields)


def data_va(base=HIGHER_HALF, offset=0):
    return base + 0x2000 + offset



# Field layouts of ELF64 and ELF32 files: header, program header and section
# header fields as (name, offset, struct format).
LAYOUTS = {
    2: {'header': (('e_type', 16, 'H'), ('e_machine', 18, 'H'), ('e_entry', 24, 'Q'), ('e_phoff', 32, 'Q'),
                   ('e_shoff', 40, 'Q'), ('e_phentsize', 54, 'H'), ('e_phnum', 56, 'H'), ('e_shentsize', 58, 'H'),
                   ('e_shnum', 60, 'H'), ('e_shstrndx', 62, 'H')),
        'phdr': (('p_type', 0, 'I'), ('p_flags', 4, 'I'), ('p_offset', 8, 'Q'), ('p_vaddr', 16, 'Q'),
                 ('p_paddr', 24, 'Q'), ('p_filesz', 32, 'Q'), ('p_memsz', 40, 'Q'), ('p_align', 48, 'Q')),
        'shdr': (('sh_name', 0, 'I'), ('sh_type', 4, 'I'), ('sh_flags', 8, 'Q'), ('sh_addr', 16, 'Q'),
                 ('sh_offset', 24, 'Q'), ('sh_size', 32, 'Q'), ('sh_link', 40, 'I'), ('sh_info', 44, 'I'))},
    1: {'header': (('e_type', 16, 'H'), ('e_machine', 18, 'H'), ('e_entry', 24, 'I'), ('e_phoff', 28, 'I'),
                   ('e_shoff', 32, 'I'), ('e_phentsize', 42, 'H'), ('e_phnum', 44, 'H'), ('e_shentsize', 46, 'H'),
                   ('e_shnum', 48, 'H'), ('e_shstrndx', 50, 'H')),
        'phdr': (('p_type', 0, 'I'), ('p_offset', 4, 'I'), ('p_vaddr', 8, 'I'), ('p_paddr', 12, 'I'),
                 ('p_filesz', 16, 'I'), ('p_memsz', 20, 'I'), ('p_flags', 24, 'I'), ('p_align', 28, 'I')),
        'shdr': (('sh_name', 0, 'I'), ('sh_type', 4, 'I'), ('sh_flags', 8, 'I'), ('sh_addr', 12, 'I'),
                 ('sh_offset', 16, 'I'), ('sh_size', 20, 'I'), ('sh_link', 24, 'I'), ('sh_info', 28, 'I'))},
}


class ElfFile(ElfImage):
    """A linked ELF64 or ELF32 file, with fields for its header, program
    headers as p_vaddr0 and the like, and section headers as sh_offset3 and
    the like."""

    def __init__(self, data):
        layout = LAYOUTS[data[4]]
        fields = {'ei_class': (4, 'B'), 'ei_data': (5, 'B')}
        fields.update({name: (off, fmt) for name, off, fmt in layout['header']})
        self.fields = fields
        self.data = bytes(data)
        phoff, phentsize, phnum = self.field('e_phoff'), self.field('e_phentsize'), self.field('e_phnum')
        for i in range(phnum):
            for name, off, fmt in layout['phdr']:
                fields[f'{name}{i}'] = (phoff + i * phentsize + off, fmt)
        self.phdrs = [{name: self.field(f'{name}{i}') for name, _, _ in layout['phdr']} for i in range(phnum)]
        shoff, shentsize, shnum = self.field('e_shoff'), self.field('e_shentsize'), self.field('e_shnum')
        for i in range(shnum):
            for name, off, fmt in layout['shdr']:
                fields[f'{name}{i}'] = (shoff + i * shentsize + off, fmt)
        self.shstrndx = self.field('e_shstrndx')
        strtab = self.field(f'sh_offset{self.shstrndx}')
        self.sections = {}
        for i in range(shnum):
            start = strtab + self.field(f'sh_name{i}')
            self.sections[data[start:data.index(b'\0', start)].decode()] = i
        super().__init__(data, fields)

    def field(self, name):
        off, fmt = self.fields[name]
        return struct.unpack_from('<' + fmt, self.data, off)[0]

    def section(self, name):
        """(index, file offset, size) of a section."""
        i = self.sections[name]
        return i, self.field(f'sh_offset{i}'), self.field(f'sh_size{i}')

    def loads(self):
        """Indices of the PT_LOAD program headers."""
        return [i for i, p in enumerate(self.phdrs) if p['p_type'] == PT_LOAD]

    def with_bytes(self, offset, data):
        out = bytearray(self.data)
        out[offset:offset + len(data)] = data
        return bytes(out)

    def symbols(self):
        """{name: (value, size)} from the symbol table, for ELF64 files."""
        i = self.sections['.symtab']
        off, size, link = self.field(f'sh_offset{i}'), self.field(f'sh_size{i}'), self.field(f'sh_link{i}')
        strtab = self.field(f'sh_offset{link}')
        out = {}
        for at in range(off, off + size, 24):
            name, _, _, _, value, sym_size = struct.unpack_from('<IBBHQQ', self.data, at)
            if name:
                out[self.data[strtab + name:self.data.index(b'\0', strtab + name)].decode()] = (value, sym_size)
        return out

    def offset_of(self, vaddr):
        """The file offset of a virtual address with file contents."""
        for i in self.loads():
            p = self.phdrs[i]
            if p['p_vaddr'] <= vaddr < p['p_vaddr'] + p['p_filesz']:
                return p['p_offset'] + vaddr - p['p_vaddr']
        raise ValueError(f'{vaddr:#x} has no file contents')
