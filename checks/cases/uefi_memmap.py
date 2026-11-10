import re

from harness.api import check
from harness.media import HostFile
from harness.ports import UEFI_PORTS
from cases._common import check_report_clean, entry, kernel_file

# How UEFI Limine takes the firmware's memory map.


def memmap_types(r, base, size):
    """The types of the memory map entries overlapping [base, base + size)."""
    types = set()
    for value in r.indexed('memmap'):
        entry_base, length, kind = value.split()
        entry_base, length = int(entry_base, 16), int(length, 16)
        if entry_base < base + size and base < entry_base + length:
            types.add(kind)
    return types


@check(ports=UEFI_PORTS, tags=['limine-protocol', 'memory'])
def conventional_memory_attributes(t):
    """Conventional memory earmarked for specific purposes, or whose
    cacheability attributes leave out write-back, is not usable memory (UEFI
    2.11 section 7.2.3) [spec]; with write-back, or with no cacheability
    attribute at all, it is [intent]. The EFI probe has GetMemoryMap() give
    pages it holds those attributes."""
    from cases.chainload import probe_then_limine
    config = 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x')
    files = {'/boot/kernel.elf': HostFile(kernel_file(t, 'full'))}
    # EFI_MEMORY_SP, and UC, WC, WT and WB.
    for attributes, usable in ((0x4000f, False), (0x1, False), (0xf, True), (0x0, True)):
        m = probe_then_limine(t, f'name:memattr memattr:{attributes:x}', config, files)
        r = t.kernel_report(m)
        check_report_clean(t, r)
        found = re.search(r'K memattr=([0-9a-f]+) ([0-9a-f]+)\r?\n', m.stream(t.kernel_channel).text())
        t.require(found is not None, f'attributes {attributes:x}: the probe set no pages up')
        types = memmap_types(r, int(found.group(1), 16), int(found.group(2), 16))
        want = {'usable'} if usable else {'reserved'}
        t.expect_eq(types, want, f'attributes {attributes:x}: the pages in the memory map')
        m.stop()
