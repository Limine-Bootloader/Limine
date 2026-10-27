import struct

from harness.api import check
from cases._common import boot_entry, entry

# Where BIOS Limine's serial output goes (CONFIG.md's serial option): the
# UART the ACPI SPCR table names (Microsoft's Serial Port Console Redirection
# Table specification, table revision 4), if it is one Limine can drive and
# it answers, else COM1 as the BIOS data area gives it. QEMU builds no SPCR
# for x86, so the checks give it one with -acpitable; COM2 writes to a file.

LOADING = 'limine: Loading executable'
IO, MEMORY = 1, 0


def spcr(revision=2, interface=0, space=IO, address=0x2f8, clock=None):
    body = struct.pack('<B3sBBBBQ', interface, bytes(3), space, 8, 0, 1, address)
    body += struct.pack('<BBIBBBBBBHHBBBIB', 0, 3, 0, 7, 0, 1, 0, 0, 0, 0xffff, 0xffff, 0, 0, 0, 0, 0)
    if revision >= 3:
        body += struct.pack('<I', clock or 1843200)
    length = 36 + len(body)
    header = struct.pack('<4sIBB6s8sI4sI', b'SPCR', length, revision, 0, b'LIMINE', b'TESTSPCR', 1, b'LMNT', 1)
    table = bytearray(header + body)
    table[9] = -sum(table) & 0xff
    return bytes(table)


def boot_with_spcr(t, table):
    path, com2 = t.path('spcr.bin'), t.path('com2.log')
    with open(path, 'wb') as f:
        f.write(table)
    config = 'timeout: 0\nserial: yes\nverbose: yes\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x')
    m, _ = boot_entry(t, config, extra=['-acpitable', f'file={path}', '-chardev', f'file,id=com2,path={com2}',
                                        '-serial', 'chardev:com2'])
    t.kernel_report(m)
    m.stop()
    with open(com2, errors='replace') as f:
        return m.stream('serial').text(), f.read()


@check(ports=['bios'], tags=['console', 'serial'])
def serial_spcr(t):
    """The SPCR's UART is used: COM2 by I/O port at revision 2, and at
    revision 3 with the clock it states [spec]. One Limine cannot drive or
    trust (another interface type, an I/O port below 0x100 or a memory
    address below 1MiB, the wrong space, MMIO over RAM) or that does not
    answer leaves COM1 to be used [intent]."""
    for what, table in (('revision 2', spcr()), ('revision 3', spcr(revision=3, clock=1843200 * 2))):
        com1, com2 = boot_with_spcr(t, table)
        t.check(LOADING in com2 and LOADING not in com1, f'{what}: output on COM2, as the SPCR says')
    for what, table in (('PL011', spcr(interface=3)),
                        ('port below 0x100', spcr(address=0x80)),
                        ('memory below 1MiB', spcr(space=MEMORY, address=0x3f8)),
                        ('MMIO over RAM', spcr(space=MEMORY, address=0x2000000)),
                        ('MMIO with no UART', spcr(space=MEMORY, address=0xfe000000)),
                        ('a port with no UART', spcr(address=0x2e8))):
        com1, com2 = boot_with_spcr(t, table)
        t.check(LOADING in com1 and LOADING not in com2, f'{what}: SPCR passed over, output on COM1')
