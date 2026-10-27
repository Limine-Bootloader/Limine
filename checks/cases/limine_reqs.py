import struct

from harness.api import check
from harness.elf import ElfFile
from harness.ports import ALL_PORTS
from cases._common import boot_entry, check_report_clean, entry, kernel_file, panics_in_turn, read

# PROTOCOL.md's Limine Requests Section: an executable asking for base
# revision 0 may list its requests in .limine_reqs, a NULL-terminated array
# of pointers to them, and only those are answered; from base revision 1 on
# the section means nothing. The reqs executables list every request but the
# date at boot one, and reqs0 holds a second bootloader info request as well,
# which the list leaves out, but a scan of the image would find and refuse.
# aarch64 boots nothing below base revision 6.

PORTS = [p for p in ALL_PORTS if p != 'uefi-aarch64']
CONFLICT = 'limine: Conflict detected for request ID 0xf55038d8e2a1202f 0x279426fcf5f59740'
OUTSIDE = 'limine: .limine_reqs entry outside kernel image'


def boot_report(t, variant):
    m, _ = boot_entry(t, 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x'),
                      kernel_variant=variant)
    report = t.kernel_report(m)
    check_report_clean(t, report)
    m.stop()
    return report


@check(ports=PORTS, tags=['limine-protocol', 'elf'])
def limine_reqs_section(t):
    """At base revision 0 the requests are the ones .limine_reqs lists [spec],
    pointers that relocation moves like any other in a relocatable executable;
    the image is not scanned, so the unlisted duplicate goes unseen [intent]."""
    for variant in ('reqs0', 'piereqs0'):
        r = boot_report(t, variant)
        t.expect_eq(r.int('base_revision.effective'), 0, f'{variant}: base revision 0')
        t.expect_eq(r.int('date_at_boot.response'), 0, f'{variant}: the unlisted date at boot request unanswered')
        for listed in ('bootloader_info', 'hhdm', 'memmap', 'executable_address', 'entropy'):
            t.expect_eq(r.int(f'{listed}.response'), 1, f'{variant}: listed {listed} request answered')


@check(ports=PORTS, tags=['limine-protocol', 'elf'])
def limine_reqs_ignored(t):
    """From base revision 1 on, .limine_reqs is not looked at: the date at
    boot request it leaves out is found and answered all the same [spec]."""
    r = boot_report(t, 'reqs1')
    t.expect_eq(r.int('base_revision.effective'), 1, 'base revision 1')
    t.expect_eq(r.int('date_at_boot.response'), 1, 'the unlisted date at boot request answered')


def reqs_cases(t):
    e = ElfFile(read(kernel_file(t, 'reqs0')))
    reqs, offset, size = e.section('.limine_reqs')
    strtab = e.shstrndx
    shnum = e.field('e_shnum')
    first = e.field(f'sh_name{reqs}')
    end = max(e.phdrs[i]['p_vaddr'] + e.phdrs[i]['p_filesz'] for i in e.loads())
    entry0, = struct.unpack_from('<Q', e.data, offset)

    def entry_at(value):
        return e.with_bytes(offset, struct.pack('<Q', value))

    # A section header table the section cannot be found through: the image
    # is scanned instead [intent], which finds the duplicate.
    fallback = [
        ('section data past the end', e.patched(**{f'sh_offset{reqs}': len(e.data)})),
        ('section larger than the file', e.patched(**{f'sh_size{reqs}': len(e.data)})),
        ('section name past the string table', e.patched(**{f'sh_name{reqs}': 0xffffff})),
        ('section name cut short', e.patched(**{f'sh_size{strtab}': first + 3})),
        ('string table index out of range', e.patched(e_shstrndx=shnum)),
        ('no string table', e.patched(e_shstrndx=0)),
        ('string table past the end', e.patched(**{f'sh_offset{strtab}': len(e.data)})),
        ('section table past the end', e.patched(e_shoff=len(e.data) - 64)),
        ('no section table', e.patched(e_shoff=0, e_shnum=0)),
        # ELF gABI 4.3 extended section numbering, its count in section 0
        # beyond what the ELF64 header field could hold, or headers too short.
        ('section count beyond 32 bits', e.patched(e_shnum=0, sh_size0=1 << 32)),
        ('extended numbering with short headers', e.patched(e_shnum=0, sh_size0=shnum, e_shentsize=32)),
        ('extended numbering past the end', e.patched(e_shnum=0, sh_size0=0x10000)),
    ]
    cases = [(what, image, CONFLICT) for what, image in fallback]
    cases += [
        ('entry below the image', entry_at(0x1000), OUTSIDE),
        ('entry past the image', entry_at(end + 0x1000), OUTSIDE),
        ('entry straddling the end of the image', entry_at((end - 16) & ~7), OUTSIDE),
        ('entry not 8-byte aligned', entry_at(entry0 + 4), 'limine: .limine_reqs entry is not 8-byte aligned'),
        ('short section headers', e.patched(e_shentsize=32), 'elf: shdr_size < sizeof(struct elf64_shdr)'),
        ('more than 128 entries', read(kernel_file(t, 'reqsmax')), 'limine: Maximum requests exceeded'),
    ]
    return cases


@check(ports=PORTS, tags=['limine-protocol', 'elf', 'hostile'], timeout=600)
def limine_reqs_hostile(t):
    """A .limine_reqs that cannot be found leaves the image to be scanned; one
    pointing outside the image or off 8 bytes, or holding more requests than
    the bootloader keeps, is refused."""
    cases = reqs_cases(t)
    files, entries = {}, ''
    for i, (_, image, _) in enumerate(cases):
        files[f'/boot/r{i:02}.elf'] = image
        entries += entry(f'R{i:02}', path=f'boot():/boot/r{i:02}.elf')
    m, _ = boot_entry(t, 'timeout: no\n' + entries, files=files, kernel_variant=None)
    msgs = panics_in_turn(t, m, len(cases))
    for (what, _, want), got in zip(cases, msgs):
        t.check(got == want, f'{what}: {got!r}, want {want!r}')


@check(ports=PORTS, tags=['limine-protocol', 'elf'])
def limine_reqs_extended_numbering(t):
    """The section is found through ELF gABI 4.3 extended section numbering,
    the count in section 0's sh_size, the string table index in its sh_link."""
    e = ElfFile(read(kernel_file(t, 'reqs0')))
    image = e.patched(e_shnum=0, sh_size0=e.field('e_shnum'), e_shstrndx=0xffff, sh_link0=e.shstrndx)
    m, _ = boot_entry(t, 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x'),
                      files={'/boot/kernel.elf': image}, kernel_variant=None)
    r = t.kernel_report(m)
    check_report_clean(t, r)
    t.expect_eq(r.int('date_at_boot.response'), 0, 'requests taken from the list')
