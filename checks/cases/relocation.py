from harness.api import check
from harness.ports import ALL_PORTS
from harness.elf import ElfFile
from cases._common import boot_kernel, check_report_clean, entry, kernel_file, read

# Relocatable executables, as PROTOCOL.md's "Memory Layout at Entry" has them,
# and CONFIG.md's kaslr and randomise_hhdm_base options. The pie check
# executable is linked at 0, and checks itself that it runs where the
# executable address response says.

HIGHER_HALF = 0xffffffff80000000


def layout(t, variant='pie', options=''):
    """(virtual base, HHDM offset) of one boot."""
    r, m, _ = boot_kernel(t, variant, entry_text=entry('Test', path='boot():/boot/kernel.elf', cmdline='x',
                                                       extra=options))
    check_report_clean(t, r)
    m.stop()
    return int(r['executable_address.virtual_base'], 16), int(r['hhdm.offset'], 16)


def layouts(t, n, **kwargs):
    return [layout(t, **kwargs) for _ in range(n)]


@check(ports=ALL_PORTS, tags=['limine', 'relocation'])
def relocatable_slide(t):
    """A relocatable executable linked in the lower half runs in the higher
    half, slid by at least 0xffffffff80000000 less its lowest address; with
    KASLR off by default, the same way each boot, the HHDM base too."""
    runs = layouts(t, 2)
    t.check(all(base >= HIGHER_HALF for base, _ in runs), f'[spec] slid into the higher half: {runs}')
    t.expect_eq(runs[0], runs[1], '[spec] no randomisation by default')


@check(ports=ALL_PORTS, tags=['limine', 'relocation'])
def relocatable_kaslr(t):
    """kaslr: yes randomises the slide, and with it by default the HHDM
    base."""
    runs = layouts(t, 3, options='kaslr: yes')
    t.check(all(base >= HIGHER_HALF for base, _ in runs), f'[spec] still in the higher half: {runs}')
    t.check(len({base for base, _ in runs}) > 1, f'[spec] slides randomised: {runs}')
    t.check(len({hhdm for _, hhdm in runs}) > 1, f'[spec] HHDM bases randomised: {runs}')


@check(ports=ALL_PORTS, tags=['limine', 'relocation'])
def relocatable_hhdm_options(t):
    """randomise_hhdm_base sets whether the HHDM base is randomised, KASLR
    or not."""
    fixed = layouts(t, 3, options='kaslr: yes\nrandomise_hhdm_base: no')
    t.check(len({base for base, _ in fixed}) > 1, f'[spec] slides randomised: {fixed}')
    t.check(len({hhdm for _, hhdm in fixed}) == 1, f'[spec] HHDM base fixed: {fixed}')
    t.expect_eq(fixed[0][1], layout(t)[1], '[intent] the HHDM base KASLR does not move')
    free = layouts(t, 3, options='randomize_hhdm_base: yes')
    t.check(len({base for base, _ in free}) == 1, f'[spec] no KASLR, one slide: {free}')
    t.check(len({hhdm for _, hhdm in free}) > 1, f'[spec] HHDM bases randomised by the alias: {free}')


@check(ports=ALL_PORTS, tags=['limine', 'relocation'])
def kaslr_needs_relocatable(t):
    """kaslr applies to relocatable executables only: one linked for the
    higher half stays where it asked to be."""
    runs = layouts(t, 2, variant='full', options='kaslr: yes')
    t.expect_eq(runs[0][0], runs[1][0], '[spec] not slid')
    t.expect_eq(runs[0][0], HIGHER_HALF, '[spec] at its link address')


@check(ports=ALL_PORTS, tags=['limine', 'relocation'])
def relocatable_relr(t):
    """DT_RELR relocations (ELF gABI 4.3) are applied, slid or not."""
    if '.relr.dyn' not in ElfFile(read(kernel_file(t, 'pierelr'))).sections:
        t.skip('the linker that built the executable does not pack relative relocations')
    for options in ('', 'kaslr: yes'):
        base, _ = layout(t, 'pierelr', options)
        t.check(base >= HIGHER_HALF, f'[spec] {options or "no KASLR"}: slid into the higher half: {base:#x}')


@check(ports=ALL_PORTS, tags=['limine', 'relocation'])
def relocatable_shared_object(t):
    """A shared object's symbolic relocations, GLOB_DAT, JUMP_SLOT and 64-bit
    absolute ones, resolve through its dynamic symbol table (ELF gABI 4.3):
    a symbol by its slid value plus any addend, an absolute one by its value
    alone, an undefined weak one as 0."""
    r, m, _ = boot_kernel(t, 'shared', entry_text=entry('Test', path='boot():/boot/kernel.elf', cmdline='x',
                                                        extra='kaslr: yes'))
    check_report_clean(t, r)
    t.check(int(r['executable_address.virtual_base'], 16) >= HIGHER_HALF, '[spec] slid into the higher half')
    t.expect_eq(r.int('symbols.got'), 1, '[spec] GOT entry holds the slid symbol')
    t.expect_eq(r.int('symbols.addend'), 1, '[spec] 64-bit relocation adds its addend')
    t.expect_eq(r['symbols.absolute'], '1234', '[spec] absolute symbol not slid')
    t.expect_eq(r['symbols.weak'], '0', '[spec] undefined weak symbol is 0')
    t.expect_eq(r['symbols.weak_addend'], '10', '[spec] undefined weak symbol plus addend 16')
