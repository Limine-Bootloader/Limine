import hashlib
import re
import time

from harness.api import check
from harness import secureboot
from harness.tpm import firmware_supports_tpm
from harness.media import HostFile, Tree
from harness.ports import UEFI_PORTS
from harness.report import fnv1a, unescape
from cases._common import entry, kernel_file, read, screen_text, wait_panic
from cases.secureboot import kernel_entry, sb_boot

# Measured boot as USAGE.md's "Measured Boot" section has it: what Limine
# extends PCR 8 and PCR 9 with, read back from the event log the check
# executable is handed. A TPM takes the firmware's -secboot build, left
# in Setup Mode unless a check enrols keys. [intent] marks what only
# Limine's code says.

EV_IPL = 0x0000000d


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def blake2b(data):
    return hashlib.blake2b(data).hexdigest()


def tpm_firmware(t, tpm=True):
    """The port's -secboot firmware, (code, vars template), and with tpm, a
    port where it can talk to one."""
    fw = secureboot.firmware(t.env, t.port)
    if fw is None:
        t.skip('no ovmf-*-secboot.fd firmware')
    if tpm and not firmware_supports_tpm(fw[0]):
        t.skip('the -secboot firmware has no TPM support for this port')
    return fw


def tpm_boot(t, config, files=None, tpm=True):
    code, template = tpm_firmware(t, tpm)
    tree = Tree()
    for path, data in (files or {}).items():
        tree.add(path, data)
    t.limine_files(config, tree)
    return t.boot(t.boot_disk(tree), code_file=code, vars_file=template,
                  tpm=t.software_tpm() if tpm else None, **secureboot.machine_options(t.port))


def events(report):
    """(pcr, type, SHA-256, data) of each event in the log, in order."""
    out = []
    for i in range(report.int('tpm.event_count')):
        pcr, kind, digest, data = report[f'tpm.event.{i}'].split(' ', 3)
        out.append((int(pcr), int(kind, 16), digest, unescape(data)))
    return out


def by_pcr(evs, pcr):
    return [(kind, digest, data) for p, kind, digest, data in evs if p == pcr]


@check(ports=UEFI_PORTS, tags=['tpm'])
def tpm_measured_boot(t):
    """measured_boot: yes extends PCR 8 with the command line and the paths
    of the executable and each module, any trailing #hash taken off; and
    PCR 9 with the config file, the executable and each module, as loaded;
    all EV_IPL events."""
    kernel = read(kernel_file(t, 'full'))
    mod1, mod2 = b'first module\n', b'second module\n'
    # A '#' of its own in the name: only the last one starts the hash.
    kpath, m1path, m2path = 'boot():/boot/k#1.elf', 'boot():/boot/m1.bin', 'boot():/boot/m2.bin'
    config = 'timeout: 0\nmeasured_boot: yes\n' + entry(
        'Test', path=f'{kpath}#{blake2b(kernel)}', cmdline='measured cmdline',
        modules=[(m1path, 'one'), (f'{m2path}#{blake2b(mod2)}', 'two')])
    m = tpm_boot(t, config, {'/boot/k#1.elf': kernel, '/boot/m1.bin': mod1, '/boot/m2.bin': mod2})
    evs = events(t.kernel_report(m, timeout=120))
    t.expect_eq(by_pcr(evs, 8), [
        (EV_IPL, sha256(b'measured cmdline'), 'cmdline: measured cmdline'),
        (EV_IPL, sha256(kpath.encode()), f'path: {kpath}'),
        (EV_IPL, sha256(m1path.encode()), f'module_path: {m1path}'),
        (EV_IPL, sha256(m2path.encode()), f'module_path: {m2path}'),
    ], '[spec] PCR 8 ([intent] the paths described without their hash)')
    t.expect_eq(by_pcr(evs, 9), [
        (EV_IPL, sha256(config.encode()), 'limine_cfg'),
        (EV_IPL, sha256(kernel), f'path: {kpath}#{blake2b(kernel)}'),
        (EV_IPL, sha256(mod1), f'module_path: {m1path}'),
        (EV_IPL, sha256(mod2), f'module_path: {m2path}#{blake2b(mod2)}'),
    ], '[spec] PCR 9 ([intent] the paths described as written)')


@check(ports=UEFI_PORTS, tags=['tpm'])
def tpm_not_opted_in(t):
    """Without measured_boot Limine extends nothing, but the firmware's own
    event log is still passed on."""
    config = 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='plain')
    m = tpm_boot(t, config, {'/boot/kernel.elf': HostFile(kernel_file(t, 'full'))})
    r = t.kernel_report(m, timeout=120)
    t.check(r.flag('tpm_event_log.response'), '[spec] the firmware\'s event log passed on')
    evs = events(r)
    t.check(evs and all(pcr < 8 for pcr, _, _, _ in evs), f'[spec] firmware events only: {[e[0] for e in evs]}')


@check(ports=UEFI_PORTS, tags=['tpm'])
def tpm_absent(t):
    """measured_boot: yes with no TPM measures nothing and boots as usual."""
    config = 'timeout: 0\nmeasured_boot: yes\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='plain')
    m = tpm_boot(t, config, {'/boot/kernel.elf': HostFile(kernel_file(t, 'full'))}, tpm=False)
    r = t.kernel_report(m, timeout=120)
    t.expect_eq(unescape(r['cmdline']), 'plain', 'booted')
    t.check(not r.flag('tpm_event_log.response'), '[spec] no event log without a TPM')


@check(ports=UEFI_PORTS, tags=['tpm'])
def tpm_panic_halts(t):
    """Under measured boot a panic halts rather than offering the menu again."""
    config = 'timeout: 0\nmeasured_boot: yes\n' + entry('Test', path='boot():/boot/missing.elf')
    m = tpm_boot(t, config)
    _, scr = wait_panic(t, m)
    text = screen_text(scr)
    t.check('System halted.' in text and 'Press a key' not in text, f'[spec] halted:\n{text}')


@check(ports=UEFI_PORTS, tags=['tpm', 'secureboot'])
def tpm_forced_by_secure_boot(t):
    """With Secure Boot active and a config hash enrolled, measured boot is
    on without measured_boot set."""
    tpm_firmware(t)
    m = sb_boot(t, 'timeout: 0\n' + kernel_entry(t), tpm=t.software_tpm())
    evs = events(t.kernel_report(m, timeout=120))
    t.check(any(pcr == 9 and data == 'limine_cfg' for pcr, _, _, data in evs),
            f'[spec] the config measured into PCR 9: {[(e[0], e[3]) for e in evs]}')


@check(ports=UEFI_PORTS, tags=['tpm', 'linux'])
def tpm_linux_event_log(t):
    """Linux gets the firmware's event log as the LINUX_EFI_TPM_EVENT_LOG
    configuration table USAGE.md names: the crypto-agile log, version 2,
    beginning with the Spec ID event (TCG PC Client PFP 1.06 revision 52,
    10.4.5.1), and of the final events table no more than it holds [spec]."""
    from cases.linux import bzimage, stub, stub_report, x86_report
    if t.port.is_x86:
        files, path = {'/bzImage': bzimage(t)}, 'boot():/bzImage'
    else:
        files, path = {'/Image': stub(t)}, 'boot():/Image'
    config = 'timeout: 0\nmeasured_boot: yes\n' + entry('Linux', protocol='linux', path=path, cmdline='tpm')
    m = tpm_boot(t, config, files)
    r = x86_report(t, m) if t.port.is_x86 else stub_report(t, m)
    t.require('tpm_log.size' in r, 'the TPM event log table installed')
    size, preboot = int(r['tpm_log.size'], 16), int(r['tpm_log.final_events_preboot_size'], 16)
    t.expect_eq(int(r['tpm_log.version'], 16), 2, 'version 2: the crypto-agile log')
    t.check(size > 0 and preboot <= size, f'size {size}, final events preboot size {preboot}')
    t.expect_eq(unescape(r['tpm_log.first_event']), 'Spec ID Event03', 'the Spec ID event first')


CC_CALL = re.compile(r'K cc\.(map|extend)=([^\r\n]*)')
ESCAPES = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]')
CC_PROMPT = 'Press Y to continue, press any other key to panic...'


def cc_extends(text):
    """(pcr, mr, type, fnv1a, length, description) of each extend the probe's
    CC protocol saw, the PCR the one Limine mapped just before it."""
    out, pcr = [], None
    for kind, value in CC_CALL.findall(text):
        if kind == 'map':
            pcr = int(value)
        else:
            mr, kind_, _, digest, length, desc = value.split(' ', 5)
            out.append((pcr, int(mr), int(kind_, 16), int(digest, 16), int(length), unescape(desc)))
    return out


def cc_boot(t, flags, config, files):
    from cases.chainload import probe_then_limine
    return probe_then_limine(t, f'name:cc fakecc:{flags:x}', config, files)


def answer_prompts(t, m, key, done, most):
    """Answer each prompt to go on unmeasured with key until done(); return
    how many Limine showed."""
    answered = 0
    deadline = time.monotonic() + 120 * t.port.timeout_scale
    while not done():
        shown = ESCAPES.sub('', m.stream('serial').text()).count(CC_PROMPT)
        if shown > answered:
            t.require(shown <= most, f'no more than {most} prompts')
            m.press(key)
            answered = shown
        elif time.monotonic() > deadline or m.exited():
            t.fail(f'stuck after {answered} prompts')
        time.sleep(0.2)
    return answered


@check(ports=UEFI_PORTS, tags=['tpm', 'hostile'])
def tpm_cc_measurements(t):
    """With no TPM but a CC measurement protocol, here one the EFI probe
    installs, the same events go to the MR each PCR maps to (USAGE.md) [spec].
    An MR that cannot be had, or an extend that fails, stops the boot to ask
    whether to go on unmeasured, any key but Y panicking [intent]."""
    kernel = read(kernel_file(t, 'full'))
    mod = b'measured module\n'
    kpath, mpath = 'boot():/boot/kernel.elf', 'boot():/boot/m.bin'
    config = 'timeout: 0\nserial: yes\nmeasured_boot: yes\n' + entry('Test', path=kpath, cmdline='cc cmdline',
                                                                      modules=[(mpath, 'mod')])
    files = {'/boot/kernel.elf': kernel, '/boot/m.bin': mod}
    m = cc_boot(t, 0, config, files)
    r = t.kernel_report(m)
    # Firmware measures into the PCRs below 8 for itself.
    evs = [e for e in cc_extends(m.stream(t.kernel_channel).text()) if e[0] >= 8]
    want = [(8, b'cc cmdline', 'cmdline: cc cmdline'), (8, kpath.encode(), f'path: {kpath}'),
            (8, mpath.encode(), f'module_path: {mpath}'), (9, config.encode(), 'limine_cfg'),
            (9, kernel, f'path: {kpath}'), (9, mod, f'module_path: {mpath}')]
    got = sorted(evs, key=lambda e: e[0])
    t.expect_eq(got, [(pcr, 3, EV_IPL, int(fnv1a(data), 16), len(data), desc) for pcr, data, desc in want],
                '[spec] PCR 8 and 9 events, in RTMR[2], the MR they map to')
    t.check(not r.flag('tpm_event_log.response'), 'no event log from a protocol that gives none')
    m.stop()
    config = 'timeout: 0\nserial: yes\nmeasured_boot: yes\n' + entry('Test', path=kpath, cmdline='x')
    for flags, warning in ((1, 'WARNING: tpm: no measurement register for PCR 8'),
                           (2, 'WARNING: tpm: CC HashLogExtendEvent for PCR 8 (MR 3) failed')):
        m = cc_boot(t, flags, config, files)
        shown = answer_prompts(t, m, 'y', lambda: 'E DONE' in m.stream(t.kernel_channel).text(), 4)
        t.expect_eq(shown, 4, f'flags {flags}: a prompt for each of the four measurements')
        t.check(warning in ESCAPES.sub('', m.stream('serial').text()), f'flags {flags}: {warning!r}')
        m.stop()
    m = cc_boot(t, 1, config, files)
    answer_prompts(t, m, 'n', lambda: CC_PROMPT in ESCAPES.sub('', m.stream('serial').text()), 1)
    m.press('n')
    msg, scr = wait_panic(t, m)
    t.expect_eq(msg, 'tpm: refusing to boot with an unmeasured component', 'any other key panics')


TCG2_CALL = re.compile(r'K tcg2\.extend=([^\r\n]*)')
LOG_FULL_WARNING = 'WARNING: tpm: The event log is full; measurements from here on are not logged.'


def tcg2_extends(text):
    """(pcr, type, fnv1a, length, description) of each extend the probe's
    TCG2 protocol saw."""
    out = []
    for value in TCG2_CALL.findall(text):
        pcr, kind, _, digest, length, desc = value.split(' ', 5)
        out.append((int(pcr), int(kind, 16), int(digest, 16), int(length), unescape(desc)))
    return out


@check(ports=UEFI_PORTS, tags=['tpm', 'hostile'])
def tpm_event_log_full(t):
    """EFI_VOLUME_FULL from HashLogExtendEvent() says the extend took place
    and only its event went unlogged (TCG EFI Protocol Specification Level 00
    Revision 00.13, Table 12; UEFI 2.11 section 38.2.4) [spec]. Under the
    TCG2 protocol and the CC one, each a stand-in the EFI probe installs, the
    boot goes on unasked, with one warning however many extends report it
    [user]."""
    from cases.chainload import probe_then_limine
    kernel = read(kernel_file(t, 'full'))
    mod = b'measured module\n'
    kpath, mpath = 'boot():/boot/kernel.elf', 'boot():/boot/m.bin'
    config = 'timeout: 0\nserial: yes\nmeasured_boot: yes\n' + entry('Test', path=kpath, cmdline='full log',
                                                                      modules=[(mpath, 'mod')])
    files = {'/boot/kernel.elf': kernel, '/boot/m.bin': mod}
    for protocol, extends in (('tcg2', tcg2_extends), ('cc', cc_extends)):
        m = probe_then_limine(t, f'name:{protocol} fake{protocol}:8', config, files)
        t.kernel_report(m)
        pcrs = sorted(e[0] for e in extends(m.stream(t.kernel_channel).text()) if e[0] >= 8)
        t.expect_eq(pcrs, [8, 8, 8, 9, 9, 9], f'{protocol}: all six measurements extended')
        serial = ESCAPES.sub('', m.stream('serial').text())
        t.expect_eq(serial.count(LOG_FULL_WARNING), 1, f'{protocol}: one warning')
        t.check(CC_PROMPT not in serial, f'{protocol}: no prompt to go on unmeasured')
        m.stop()


@check(ports=UEFI_PORTS, tags=['tpm', 'linux'])
def tpm_cc_linux(t):
    """A Linux kernel booted under the CC measurement protocol is measured as
    USAGE.md has it: command line and paths into PCR 8, the config, the
    kernel image as read from disk and the initrd into PCR 9 [spec]. With
    the protocol's GetEventLog() failing there is no log to pass on, and
    no TPM event log table [intent]."""
    from cases.linux import bzimage, stub, stub_report, x86_report
    image = bzimage(t) if t.port.is_x86 else stub(t)
    path, kernel = ('/bzImage' if t.port.is_x86 else '/Image'), read(image.path)
    initrd = b'measured initrd\n' * 9
    config = 'timeout: 0\nmeasured_boot: yes\n' + entry('Linux', protocol='linux', path=f'boot():{path}',
                                                         cmdline='cc linux', modules=[('boot():/initrd', None)])
    m = cc_boot(t, 4, config, {path: kernel, '/initrd': initrd})
    r = x86_report(t, m) if t.port.is_x86 else stub_report(t, m)
    text = m.stream(t.kernel_channel).text()
    evs = [e for e in cc_extends(text) if e[0] >= 8]
    want = [(8, b'cc linux', 'cmdline: cc linux'), (8, f'boot():{path}'.encode(), f'path: boot():{path}'),
            (8, b'boot():/initrd', 'module_path: boot():/initrd'), (9, config.encode(), 'limine_cfg'),
            (9, kernel, f'path: boot():{path}'), (9, initrd, 'module_path: boot():/initrd')]
    t.expect_eq(sorted(evs, key=lambda e: e[0]),
                [(pcr, 3, EV_IPL, int(fnv1a(data), 16), len(data), desc) for pcr, data, desc in want],
                '[spec] PCR 8 and 9 events')
    t.check('K cc.event_log=2' in text, 'the TCG 2 log asked for')
    t.check('tpm_log.size' not in r, 'no TPM event log table')
