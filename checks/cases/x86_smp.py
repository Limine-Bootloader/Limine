import re
import struct

from harness.api import check
from harness.media import HostFile
from cases._common import boot_entry, check_report_clean, entry, kernel_file

# x86 APs the MADT lists only as local x2APICs (ACPI 6.6 5.2.12.12), their
# APIC IDs past 254: here one AP at APIC ID 256, the third socket's first
# core when each socket counts its cores in 7 bits. QEMU wants an IOMMU with
# extended interrupt remapping for such IDs, which brings a DMAR table too.

SMP = '1,maxcpus=195,sockets=3,cores=65,threads=1'
EXTRA = ['-device', 'qemu64-x86_64-cpu,socket-id=2,core-id=0,thread-id=0',
         '-device', 'intel-iommu,intremap=on,eim=on']


def boot_high_ids(t, variant):
    m, _ = boot_entry(t, 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x'),
                      kernel_variant=variant, smp=SMP, cpu='qemu64,+x2apic', machine='q35',
                      machine_opts=['kernel-irqchip=split'], extra=EXTRA)
    r = t.kernel_report(m, timeout=120)
    check_report_clean(t, r)
    m.stop()
    return r


@check(ports=['bios', 'uefi-ia32', 'uefi-x86-64'], tags=['limine-protocol', 'smp'])
def x2apic_high_apic_ids(t):
    """With the MP request's x2APIC flag the AP at APIC ID 256 is started,
    in x2APIC mode [spec]; without it, an AP xAPIC cannot address is left
    out [intent]."""
    r = boot_high_ids(t, 'x2apic')
    t.expect_eq(r['x86.apic_mode'], 'x2apic', 'x2APIC mode')
    t.expect_eq((r.int('mp.cpu_count'), r.int('mp.aps_started')), (2, 1), 'the AP at APIC ID 256 started')
    t.expect_eq(r['mp.cpu.1'].split()[:2], ['id', '100'], 'the AP listed by its APIC ID, 256')
    r = boot_high_ids(t, 'full')
    t.expect_eq(r.int('mp.cpu_count'), 1, 'without the flag: the BSP alone')
    t.expect_eq(r['x86.apic_mode'], 'xapic', '[spec] without the flag: xAPIC mode')


REVERTED = 'Firmware had x2APIC enabled, reverted to xAPIC mode'


def boot_through_probe(t, config, files, **machine):
    """On UEFI, Limine started by the EFI probe, which first reports the APIC
    base MSR as the firmware left it: Limine's own word on reverting x2APIC
    comes after boot services, out of the firmware console's reach."""
    from cases.chainload import probe_then_limine
    if not t.port.is_uefi:
        return boot_entry(t, config, files=files, kernel_variant=None, **machine)[0]
    return probe_then_limine(t, 'name:x2apic msr:1b', config, files, **machine)


@check(ports=['bios', 'uefi-ia32', 'uefi-x86-64'], tags=['smp', 'multiboot1', 'multiboot2', 'linux'])
def x2apic_reverted_without_mp_request(t):
    """Firmware that enabled x2APIC, for the AP at APIC ID 256, has it
    turned back off for kernels that cannot ask for it: a Limine protocol
    kernel without the MP request, and Multiboot 1, Multiboot 2 and Linux
    kernels [intent]."""
    from cases.linux import bzimage, x86_report
    from cases.multiboot import mb1_kernel, mb1_report, mb2_kernel, mb2_report
    kernels = (('limine', HostFile(kernel_file(t, 'nomp')), lambda m: t.kernel_report(m, timeout=120)),
               ('multiboot1', HostFile(mb1_kernel(t)), lambda m: mb1_report(t, m)),
               ('multiboot2', HostFile(mb2_kernel(t)), lambda m: mb2_report(t, m)),
               ('linux', bzimage(t), lambda m: x86_report(t, m)))
    for name, kernel, report in kernels:
        config = 'timeout: 0\nserial: yes\nverbose: yes\n' + entry(name, protocol=name, path='boot():/kernel',
                                                                       cmdline='x')
        m = boot_through_probe(t, config, {'/kernel': kernel}, smp=SMP, cpu='qemu64,+x2apic', machine='q35',
                               machine_opts=['kernel-irqchip=split'], extra=EXTRA)
        r = report(m)
        if t.port.is_uefi:
            msr = re.search(r'K msr\.1b=([0-9a-f]+)', m.stream('debugcon').text(), re.I)
            t.check(msr is not None and int(msr.group(1), 16) & (1 << 10), f'{name}: the firmware had x2APIC enabled')
        else:
            t.check(f'{name}: {REVERTED}' in m.stream('serial').text(), f'{name}: the firmware had x2APIC enabled')
        if name == 'limine':
            t.expect_eq(r['x86.apic_mode'], 'xapic', f'{name}: entered in xAPIC mode')
        else:
            t.expect_eq(int(r['apic_base'], 16) & (1 << 10), 0, f'{name}: entered in xAPIC mode')
        m.stop()


def dmar(flags):
    """A DMAR table (Intel VT-d 5.20 section 8.1) with no remapping units,
    just its flags."""
    table = bytearray(struct.pack('<4sIBB6s8sI4sIBB10s', b'DMAR', 48, 1, 0, b'LIMINE', b'TESTDMAR', 1, b'LMNT', 1,
                                  38, flags, bytes(10)))
    table[9] = -sum(table) & 0xff
    return bytes(table)


@check(ports=['bios', 'uefi-ia32', 'uefi-x86-64'], tags=['limine-protocol', 'smp'])
def x2apic_opt_out(t):
    """A DMAR table with its x2APIC opt-out flag set beside interrupt
    remapping asks software not to enable x2APIC (Intel VT-d 5.20 section
    8.1): the MP request's x2APIC flag then goes unanswered, the APs started
    in xAPIC mode. The opt-out alone, which the specification makes meaningful
    only with remapping, or remapping alone, changes nothing [spec]."""
    for flags, mode in ((0x3, 'xapic'), (0x1, 'x2apic'), (0x2, 'x2apic')):
        path = t.path(f'dmar{flags}.bin')
        with open(path, 'wb') as f:
            f.write(dmar(flags))
        m, _ = boot_entry(t, 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x'),
                          kernel_variant='x2apic', smp=2, cpu='qemu64,+x2apic', extra=['-acpitable', f'file={path}'])
        r = t.kernel_report(m, timeout=120)
        check_report_clean(t, r)
        t.expect_eq(r['x86.apic_mode'], mode, f'DMAR flags {flags}: APIC mode')
        t.expect_eq(r.int('mp.flags', 16), int(mode == 'x2apic'), f'DMAR flags {flags}: x2APIC flag in the response')
        t.expect_eq(r.int('mp.aps_started'), 1, f'DMAR flags {flags}: the AP started')
        m.stop()
