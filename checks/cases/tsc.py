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
# ACPI PM timer, PIT channel 2 and port 0x61, with stalls put where QEMU cannot
# put them, inside a port access once the device has answered.

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


def frequencies(r, prefix):
    """(scenario, found, wanted) for each scenario named prefix or under it."""
    out = []
    for key, value in r.facts.items():
        if (key == prefix or key.startswith(prefix + '.')) and not key.endswith('.port61'):
            found, wanted = value.split()
            out.append((key, int(found), int(wanted)))
    return out


def expect_close(t, r, prefix, tolerance):
    runs = frequencies(r, prefix)
    t.require(runs, f'no {prefix} scenario ran')
    for key, found, wanted in runs:
        t.check(abs(found - wanted) <= wanted * tolerance,
                f'{key}: {found} Hz, {(found - wanted) / wanted:+.4%} off {wanted} Hz')


@check(ports=['bios'], tags=['tsc'])
def tsc_calibration_stalls(t):
    """However the CPU stalls, inside the PM timer's reads or between them,
    the TSC frequency comes out within 0.02%, whether the timer's 24-bit count
    wraps or not. Against the PIT, with no PM timer, it comes out within
    0.05%, a round whose gate opened or whose OUT went high in a stall being
    run again, up to 10 rounds, and with every round stalled so, a frequency
    is still found [intent]."""
    r = simulate(t)
    expect_close(t, r, 'pm', 0.0002)
    for prefix in ('pit.clean', 'pit.stalls', 'pit.edges', 'pit.gates'):
        expect_close(t, r, prefix, 0.0005)
    (_, found, _), = frequencies(r, 'pit.all_edges')
    t.check(found != 0, 'with every PIT round stalled: no frequency')


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


@check(ports=['bios'], tags=['tsc'])
def tsc_calibration_crystal(t):
    """Where CPUID leaf 0x15 gives the TSC's ratio to the crystal clock but
    not the clock, the frequency is exactly that of the clock Table 22-95 of
    Intel SDM 325462-093 Vol. 3B gives the processor [spec], 24MHz for a Xeon
    W and 25MHz for a Xeon Scalable of the same model, where it agrees with
    the measurement to 1% [intent]."""
    r = simulate(t)
    if r['crystal'] != '1':
        t.skip('the crystal clocks are looked up on x86 builds of Limine only')
    for name in ('skylake', 'xeon_scalable', 'xeon_w', 'goldmont'):
        (key, found, wanted), = frequencies(r, f'crystal.{name}')
        t.expect_eq(found, wanted, key)
    for name, nominal in (('disagrees', 3000000000), ('other_model', 2400000000)):
        expect_close(t, r, f'crystal.{name}', 0.0002)
        (key, found, _), = frequencies(r, f'crystal.{name}')
        t.check(found != nominal, f'{key}: the nominal {nominal} Hz rather than the measured frequency')


def boot_icount(t, machine_opts):
    """The check executable booted where the TSC counts the nanoseconds of the
    virtual clock that also drives the PM timer and the PIT: QEMU's icount,
    here 16ns an instruction, which leaves a port access still quicker than
    any real one."""
    m, _ = boot_entry(t, 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x'),
                      machine_opts=machine_opts, extra=['-icount', 'shift=4,sleep=off'])
    r = t.kernel_report(m)
    check_report_clean(t, r)
    return m, r


@check(ports=['bios'], tags=['limine-protocol', 'tsc'])
def tsc_frequency_exact(t):
    """With the TSC at exactly 1GHz, the frequency given is 1GHz to 0.02%
    against the PM timer and to 0.05% against the PIT, with no ACPI [intent]."""
    for opts, tolerance in (([], 0.0002), (['acpi=off'], 0.0005)):
        m, r = boot_icount(t, opts)
        freq = r.int('tsc_frequency')
        t.check(abs(freq - 10**9) <= 10**9 * tolerance, f'machine {opts}: {freq} Hz')
        m.stop()


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
