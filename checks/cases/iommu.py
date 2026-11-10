import re
import struct

from harness.api import check
from harness.media import HostFile
from cases._common import check_report_clean, entry, kernel_file

# Remapping units that Limine turns off at hand-off without the Keep IOMMU
# request and that do not do as told: listed by a DMAR or IVRS table given to
# QEMU, at registers that memory stands in for.

def acpi_table(signature, revision, body):
    raw = bytearray(struct.pack('<4sIBB6s8sI4sI', signature, 36 + len(body), revision, 0, b'LIMINE',
                                b'TESTIOMM', 1, b'LMNT', 1) + body)
    raw[9] = -sum(raw) & 0xff
    return bytes(raw)


def dmar(base):
    """A DMAR (Intel VT-d 5.20 section 8.1) with one remapping unit, for every
    PCI device, its registers at base (section 8.3)."""
    return acpi_table(b'DMAR', 1, struct.pack('<BB10sHHBBHQ', 38, 0, bytes(10), 0, 16, 1, 0, 0, base))


def ivrs(base):
    """An IVRS (AMD IOMMU 3.11 section 5.2) with one IVHD block of type 10h,
    its registers at base (section 5.2.2.1)."""
    return acpi_table(b'IVRS', 2, struct.pack('<I8sBBHHHQHHI', 0, bytes(8), 0x10, 0, 24, 0x10, 0x40, base, 0,
                                              0, 0))


def table_file(t, name, data):
    path = t.path(name)
    with open(path, 'wb') as f:
        f.write(data)
    return path


@check(ports=['uefi-ia32', 'uefi-x86-64'], tags=['limine-protocol', 'iommu'])
def iommu_unit_not_stopping(t):
    """A remapping unit that never acts on being told to stop, here memory
    the EFI probe sets up as a unit left on, is waited for a bounded time and
    moved past [intent]: VT-d translation and then interrupt remapping are
    asked off, the second command carrying the translation the unit still
    shows, as each command changes one field of the status (Intel VT-d 5.20
    section 11.4.4.1) [spec], and queued invalidation, whose queue never
    drains, is left on (section 6.5.2) [spec]; AMD-Vi is turned off all the
    same once its engines do not stop [intent]."""
    from cases.chainload import probe_then_limine
    config = 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x')
    files = {'/boot/kernel.elf': HostFile(kernel_file(t, 'nokeepiommu'))}
    for name, table in (('vtd', dmar(0)), ('amdvi', ivrs(0))):
        path = table_file(t, f'{name}.bin', table)
        m = probe_then_limine(t, f'name:{name} fakeiommu:{name}', config, files, machine='q35',
                              extra=['-acpitable', f'file={path}'])
        r = t.kernel_report(m)
        check_report_clean(t, r)
        t.check(re.search(r'K fakeiommu=[0-9a-f]+\r?\n', m.stream('debugcon').text()),
                f'{name}: the probe set the unit up')
        if name == 'vtd':
            t.expect_eq(r['x86.vtd.0.gcmd'], '84000000', 'VT-d: the last command, interrupt remapping off')
        else:
            _, control, _, _ = r['x86.amdvi.0'].split()
            t.expect_eq(int(control, 16) & 0xffffffff, 0, 'AMD-Vi: its control register at entry')
        m.stop()
