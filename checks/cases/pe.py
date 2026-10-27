import struct

from harness.api import payload_arch, check
from harness import pe
from harness.ports import ALL_PORTS
from cases._common import boot_entry, boot_kernel, check_report_clean, entry, kernel_file, panics_in_turn, read

# PE32+ executables for the Limine protocol, which PROTOCOL.md does not
# mention, so [intent]: Limine's PE loader takes them where its ELF one takes
# ELF executables. The images are remade from the ELF check executables.

HIGHER_HALF = 0xffffffff80000000


def pe_report(t, variant, options=''):
    image = pe.from_elf(read(kernel_file(t, variant))).data
    report, m, _ = boot_kernel(t, variant, files={'/boot/kernel.pe': image},
                               entry_text=entry('PE', path='boot():/boot/kernel.pe', cmdline='pe', extra=options))
    check_report_clean(t, report)
    m.stop()
    return report


@check(ports=ALL_PORTS, tags=['limine-protocol', 'pe'])
def pe_executable(t):
    """A PE32+ executable boots as the ELF one it is remade from: its own
    checks pass, and it runs at the same place."""
    elf, m, _ = boot_kernel(t)
    m.stop()
    report = pe_report(t, 'full')
    t.expect_eq(report['executable_address.virtual_base'], elf['executable_address.virtual_base'], 'virtual base')
    t.expect_eq(report['entry'], elf['entry'], 'entered as the ELF one is')


@check(ports=ALL_PORTS, tags=['limine-protocol', 'pe', 'relocation'])
def pe_relocatable(t):
    """A relocatable PE32+ executable based in the lower half runs in the
    higher half, its base relocations applied, at the same place every boot;
    kaslr: yes moves it."""
    plain = [int(pe_report(t, 'pie')['executable_address.virtual_base'], 16) for _ in range(2)]
    t.check(plain[0] == plain[1] and plain[0] >= HIGHER_HALF, f'slid into the higher half, the same each boot: {plain}')
    slid = [int(pe_report(t, 'pie', 'kaslr: yes')['executable_address.virtual_base'], 16) for _ in range(3)]
    t.check(len(set(slid)) > 1 and min(slid) >= HIGHER_HALF, f'kaslr: randomised, in the higher half: {slid}')


def block(va, *entries):
    """A base relocation block (PE Format of 07/14/2025, "Base Relocation
    Block") of (type, offset) entries."""
    words = [kind << 12 | off for kind, off in entries]
    if len(words) % 2:
        words.append(0)
    return struct.pack(f'<II{len(words)}H', va, 8 + 2 * len(words), *words)


def section_data(prefix=b''):
    return prefix + bytes(0x2000 - len(prefix))


def hostile_images(arch):
    """(what, image, the panic expected) for each way of spoiling a small
    image, made so that it fails the check named and no other first."""
    wrong_machine = {'x86_64': 'pe: Not an x86-64 PE file', 'aarch64': 'pe: Not an ARM64 PE file',
                     'riscv64': 'pe: Not a RISC-V PE file', 'loongarch64': 'pe: Not a loongarch64 PE file'}[arch]
    unknown = 'limine: Unknown kernel executable format'
    s = pe.synthetic(arch, section_data())
    size = len(s.data)
    cases = [
        ('truncated', s.data[:0x30], unknown),
        ('bad NT signature', s.patched(Signature=0), unknown),
        ('unaligned NT headers', s.patched(e_lfanew=0x44), unknown),
        ('NT headers past the end', s.patched(e_lfanew=size), unknown),
        ('unknown machine', s.patched(Machine=0x1234), unknown),
        ('IA-32 machine', s.patched(Machine=0x14c), wrong_machine),
        ('another 64-bit machine', s.patched(Machine=0xaa64 if arch == 'x86_64' else 0x8664), wrong_machine),
        ('PE32 optional header', s.patched(Magic=0x10b), 'pe: Not a valid PE32+ file'),
        ('short optional header', s.patched(SizeOfOptionalHeader=200), 'pe: SizeOfOptionalHeader too small'),
        ('section table past the end', s.patched(NumberOfSections=0xffff),
         'pe: Section headers extend beyond file bounds'),
        ('unaligned section table', s.patched(SizeOfOptionalHeader=242),
         'pe: Section headers are not aligned in the file'),
        ('no sections', s.patched(NumberOfSections=0), 'pe: Executable has no sections'),
        ('zero alignment', s.patched(SectionAlignment=0), 'pe: SectionAlignment is zero'),
        ('alignment not a power of 2', s.patched(SectionAlignment=0x3000), 'pe: SectionAlignment is not a power of 2'),
        ('alignment below a page', s.patched(SectionAlignment=0x800), 'pe: SectionAlignment is below the page size'),
        ('fixed lower half base', s.patched(ImageBase=0x400000), 'pe: Lower half images are not allowed'),
        ('headers past the end', s.patched(SizeOfHeaders=size + 1), 'pe: SizeOfHeaders exceeds file or image size'),
        ('image too small', s.patched(SizeOfImage=0x1000), 'pe: Section 0 exceeds image bounds'),
        ('virtual size past the image', s.patched(VirtualSize1=0x10000),
         'pe: Section 1 virtual size exceeds image bounds'),
        ('sections out of order', s.patched(VirtualAddress1=0x1000), 'pe: Section 1 overlaps or is out of order'),
        ('unaligned section', s.patched(VirtualAddress1=0x1110), 'pe: Section 1 is not aligned to SectionAlignment'),
        ('section data past the end', s.patched(PointerToRawData1=size),
         'pe: Section 1 data extends beyond file bounds'),
        ('headers over the first section', s.patched(SizeOfHeaders=0x1800), 'pe: Headers overlap the first section'),
        ('base at the top of memory', s.patched(ImageBase=0xfffffffffffff000),
         'pe: Image extends past the address space'),
    ]
    imports = pe.synthetic(arch, section_data(struct.pack('<5I', 0, 0, 0, 0x2100, 0)))
    cases += [
        ('an import', imports.patched(ImportRva=0x2000, ImportSize=20), 'pe: Kernel must not have any imports'),
        ('import directory past the image', imports.patched(ImportRva=0x4000, ImportSize=20),
         'pe: Import directory VirtualAddress out of bounds'),
        ('unaligned import directory', imports.patched(ImportRva=0x2002, ImportSize=20),
         'pe: Import directory is not aligned in the image'),
    ]

    def relocated(blocks, size=None, rva=0x2000, base=pe.HIGHER_HALF):
        return pe.synthetic(arch, section_data(blocks), relocatable=True, image_base=base,
                            reloc_dir=(rva, len(blocks) if size is None else size)).data

    cases += [
        ('relocation directory past the image', relocated(block(0x1000, (10, 0)), rva=0x4000),
         'pe: Relocation directory VirtualAddress out of bounds'),
        ('relocation block too short', relocated(struct.pack('<II', 0x1000, 4)), 'pe: Invalid relocation block size'),
        ('relocation block past its directory', relocated(struct.pack('<II', 0x1000, 0x100) + bytes(4), size=12),
         'pe: Relocation block size exceeds directory'),
        ('relocation block past the image', relocated(block(0x4000, (10, 0))),
         'pe: Relocation block VirtualAddress out of bounds'),
        ('unsupported relocation', relocated(block(0x1000, (4, 0))), 'pe: Unsupported relocation type 4'),
        ('relocation past the image', relocated(block(0x3000, (10, 0xffc))), 'pe: Relocation offset out of bounds'),
        ('unaligned relocation block', relocated(bytes(2) + block(0x1000, (10, 0)), rva=0x2002),
         'pe: Relocation block is not aligned in the image'),
        ('32-bit relocation, slid up', relocated(block(0x1000, (3, 0)), base=0x400000),
         'pe: 32-bit relocations are incompatible with higher-half loading'),
    ]
    return cases


@check(ports=ALL_PORTS, tags=['limine-protocol', 'pe', 'hostile'], timeout=900)
def pe_hostile(t):
    """Spoilt images end in the panic for what is wrong with them, never a
    crash, and the menu comes back."""
    cases = hostile_images(payload_arch(t.port))
    files, entries = {}, ''
    for i, (_, image, _) in enumerate(cases):
        files[f'/boot/h{i:02}.pe'] = image
        entries += entry(f'H{i:02}', path=f'boot():/boot/h{i:02}.pe')
    m, _ = boot_entry(t, 'timeout: no\n' + entries, files=files)
    msgs = panics_in_turn(t, m, len(cases))
    for (what, _, want), got in zip(cases, msgs):
        t.check(got == want, f'{what}: {got!r}, want {want!r}')
