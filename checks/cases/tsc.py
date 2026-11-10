import os
import re
import shlex
import shutil
import subprocess

from harness.api import check
from harness.env import SRCDIR
from harness.report import Report
from cases._common import boot_entry, check_report_clean, entry

# BIOS Limine's TSC calibration, the C of common/sys/cpu.s2.c, built for the
# build machine and run on the machine checks/sim/tsc.c simulates: a TSC, an
# ACPI PM timer, PIT channel 2 and port 0x61.

SIM = os.path.join(SRCDIR, 'checks', 'sim')


def build_cc(t):
    """The build machine's compiler, as the build recorded it."""
    try:
        with open(os.path.join(t.env.builddir, 'tools-build', 'CC_FOR_BUILD')) as f:
            cc = shlex.split(f.read())
    except OSError:
        cc = []
    if not cc or shutil.which(cc[0]) is None:
        cc = [os.environ.get('CC_FOR_BUILD') or 'cc']
    if shutil.which(cc[0]) is None:
        t.skip('no compiler for the build machine')
    return cc


def simulate(t):
    exe = t.path('tsc')
    res = subprocess.run(build_cc(t) + ['-std=gnu11', '-O2', '-DBIOS', '-I', os.path.join(SIM, 'stubs'),
                                        os.path.join(SIM, 'tsc.c'),
                                        os.path.join(SRCDIR, 'common', 'sys', 'cpu.s2.c'), '-o', exe],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    t.require(res.returncode == 0, 'building the simulation failed:\n' + res.stdout.decode(errors='replace'))
    res = subprocess.run([exe], stdout=subprocess.PIPE, timeout=120)
    r = Report(res.stdout.decode(), 'B TSC-SIM')
    t.require(r.done is not None, 'the simulation did not finish')
    t.check('panic' not in r, f'calibrate_tsc() panicked: {r.get("panic")}')
    return r


@check(ports=['bios'], tags=['tsc'])
def tsc_calibration_port61(t):
    """The PIT calibration writes port 0x61 with bits 7:4, read-only status,
    as 0 (Intel ICH10 319973-003, 13.7.1) [spec], and leaves bits 3:0 as it
    found them [intent]."""
    r = simulate(t)
    runs = r.prefixed('pit.')
    t.require(any(key.endswith('.port61') for key in runs), 'no PIT scenario wrote port 0x61')
    for key, value in runs.items():
        if key.endswith('.port61'):
            writes, reserved, last = value.split()
            t.expect_eq(int(reserved), 0, f'{key}: writes of {writes} with bits 7:4 set')
            t.expect_eq(last, '0d', f'{key}: the value left')


# QEMU's trace of a write to a device's registers, of the PIT's and of port
# 0x61 among them: "... addr 0x61 value 0x1 size 1 name 'pcspk'".
TRACED_WRITE = re.compile(r"addr 0x([0-9a-f]+) value 0x([0-9a-f]+) size 1 name '(?:pit|pcspk)'")


@check(ports=['bios'], tags=['limine-protocol', 'tsc'])
def tsc_pit_port61(t):
    """With no ACPI, and so no PM timer, the PIT calibration writes port 0x61
    with bits 7:4, read-only status, as 0 (Intel ICH10 319973-003, 13.7.1)
    [spec]. Limine's writes are those QEMU traces from the closing of the
    gate before its first count of 11932 (0x2e9c) on [intent]."""
    trace = t.path('writes.log')
    m, _ = boot_entry(t, 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x'),
                      machine_opts=['acpi=off'], extra=['-trace', f'memory_region_ops_write,file={trace}'])
    r = t.kernel_report(m)
    check_report_clean(t, r)
    m.stop()
    writes = []
    with open(trace, errors='replace') as f:
        for line in f:
            found = TRACED_WRITE.search(line)
            if found is not None:
                writes.append((int(found.group(1), 16), int(found.group(2), 16)))
    os.unlink(trace)
    if not writes:
        t.skip('QEMU traced no writes: built without its log trace backend')
    first = next((i for i in range(len(writes) - 3) if writes[i][0] == 0x61
                  and writes[i + 1:i + 4] == [(0x43, 0xb0), (0x42, 0x9c), (0x42, 0x2e)]), None)
    t.require(first is not None, 'no PIT calibration traced')
    port61 = [value for port, value in writes[first:] if port == 0x61]
    t.check(len(port61) >= 7, f'{len(port61)} writes to port 0x61, fewer than 3 rounds and the restore take')
    t.expect_eq([f'{value:02x}' for value in port61 if value & 0xf0], [], 'writes with bits 7:4 set')
