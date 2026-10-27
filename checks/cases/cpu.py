from harness.api import check
from harness.ports import X86_PORTS
from cases._common import boot_entry, boot_kernel, check_report_clean, entry, screen_text, wait_panic

# What Limine makes of the x86 CPU features it looks for.


@check(ports=['bios', 'uefi-ia32'], tags=['limine-protocol', 'cpu'])
def cpu_without_long_mode(t):
    """On a CPU without long mode the menu's branding names IA-32, and a
    Limine protocol entry is refused [intent]."""
    m, _ = boot_entry(t, 'timeout: no\n' + entry('Test', path='boot():/boot/kernel.elf'), cpu='qemu64,-lm')
    scr = t.wait_text(m, 'ARROWS')
    want = '(ia-32, BIOS)' if t.port.name == 'bios' else '(ia-32, UEFI32)'
    t.check(want in screen_text(scr), f'branding names {want}')
    m.press('ret')
    msg, _ = wait_panic(t, m)
    t.expect_eq(msg, 'limine: This CPU does not support 64-bit mode.', 'Limine protocol refused')


@check(ports=X86_PORTS, tags=['limine-protocol', 'cpu'])
def cpu_without_nx(t):
    """Without NX the executable is entered with EFER.NXE clear, and no page
    marked no-execute (PROTOCOL.md's x86-64 machine state asks for NXE only
    where it exists) [spec]."""
    r, m, _ = boot_kernel(t, machine_kwargs={'cpu': 'qemu64,-nx'})
    check_report_clean(t, r)
    t.expect_eq(r.flag('x86.nx'), False, 'the payload sees no NX')
