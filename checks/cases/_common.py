import re
import subprocess
import time

from harness import screen as screen_mod
from harness.api import SETTLE_INTERVAL, payload_arch
from harness.media import HostFile, Tree
from harness.report import fnv1a, unescape

FIRMWARE_TYPE = {'bios': 0, 'uefi-ia32': 1}


def firmware_type(port):
    return FIRMWARE_TYPE.get(port.name, 2)


def kernel_file(t, variant='full'):
    return t.env.payload('limine', payload_arch(t.port), variant + '.elf')


def read(path):
    with open(path, 'rb') as f:
        return f.read()


def limine_version(env):
    out = subprocess.run([env.bin('limine'), '--version'], stdout=subprocess.PIPE).stdout.decode()
    m = re.search(r'Limine (\S+)', out)
    return m.group(1) if m else out.strip()


def entry(title, protocol='limine', path=None, cmdline=None, modules=(), extra='', indent='    '):
    """One menu entry as config text. modules: (path, string or None) pairs."""
    lines = [f'/{title}', f'{indent}protocol: {protocol}']
    if path is not None:
        lines.append(f'{indent}path: {path}')
    if cmdline is not None:
        lines.append(f'{indent}cmdline: {cmdline}')
    for mpath, mstring in modules:
        lines.append(f'{indent}module_path: {mpath}')
        if mstring is not None:
            lines.append(f'{indent}module_string: {mstring}')
    for line in extra.strip('\n').split('\n'):
        if line:
            lines.append(indent + line.strip())
    return '\n'.join(lines) + '\n'


def boot_kernel(t, variant='full', config_globals='timeout: 0\n', entry_text=None, files=None,
                cmdline='hello world', modules=None, smp=1, memory=None, timeout=90, disk_kwargs=None,
                machine_kwargs=None):
    """Boot a Limine protocol check executable from a disk and return
    (report, machine, disk)."""
    tree = Tree()
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, variant)))
    if modules is None:
        modules = [('/boot/mod1.bin', b'first module contents\n' * 100, 'first module')]
    mod_entries = []
    for mpath, data, mstring in modules:
        if data is not None:
            tree.add(mpath, data)
        mod_entries.append((f'boot():{mpath}', mstring))
    for path, data in (files or {}).items():
        tree.add(path, data)
    if entry_text is None:
        entry_text = entry('Test', path='boot():/boot/kernel.elf', cmdline=cmdline, modules=mod_entries)
    config = config_globals + entry_text
    t.limine_files(config, tree)
    disk = t.boot_disk(tree, **(disk_kwargs or {}))
    m = t.boot(disk, smp=smp, memory=memory, **(machine_kwargs or {}))
    report = t.kernel_report(m, timeout=timeout)
    return report, m, disk


def check_report_clean(t, report):
    """The payload's own specification checks all passed and it finished."""
    t.require(report.done is not None, 'payload did not finish its report')
    t.check(not report.failures, 'payload checks failed:\n  ' + '\n  '.join(report.failures))


def check_file(t, report, prefix, *, path, data, string, partition=1, disk=None, media_type=0):
    """A limine_file as the protocol describes it, against what is on disk."""
    t.expect_eq(unescape(report[prefix + '.path']), path, prefix + '.path')
    t.expect_eq(unescape(report[prefix + '.string']), string, prefix + '.string')
    t.expect_eq(report.int(prefix + '.size'), len(data), prefix + '.size')
    t.expect_eq(report[prefix + '.hash'], fnv1a(data), prefix + '.hash (contents)')
    t.expect_eq(report.int(prefix + '.media_type'), media_type, prefix + '.media_type')
    t.expect_eq(report.int(prefix + '.partition_index'), partition, prefix + '.partition_index')
    if disk is not None:
        if disk.scheme == 'gpt':
            t.expect_eq(report[prefix + '.gpt_disk_uuid'].lower(), disk.disk_guid.lower(),
                        prefix + '.gpt_disk_uuid')
            if partition != 0:
                t.expect_eq(report[prefix + '.gpt_part_uuid'].lower(),
                            disk.parts[partition - 1].guid.lower(), prefix + '.gpt_part_uuid')
        else:
            t.expect_eq(report[prefix + '.gpt_disk_uuid'], '00000000-0000-0000-0000-000000000000',
                        prefix + '.gpt_disk_uuid')
            t.expect_eq(report[prefix + '.gpt_part_uuid'], '00000000-0000-0000-0000-000000000000',
                        prefix + '.gpt_part_uuid')
        if disk.scheme == 'mbr':
            t.expect_eq(report.hex(prefix + '.mbr_disk_id'), disk.mbr_id, prefix + '.mbr_disk_id')


def descriptor(raw):
    """A GDT segment descriptor's fields (Intel SDM 325462-093 vol. 3A,
    figure 3-8)."""
    limit = (raw & 0xffff) | ((raw >> 48) & 0xf) << 16
    flags = (raw >> 52) & 0xf
    if flags & 8:
        limit = (limit << 12) | 0xfff
    access = (raw >> 40) & 0xff
    return {'base': ((raw >> 16) & 0xffffff) | ((raw >> 56) & 0xff) << 24, 'limit': limit,
            'present': access >> 7, 's': (access >> 4) & 1, 'type': access & 0xf, 'l': (flags >> 1) & 1,
            'db': (flags >> 2) & 1}


def screen_text(scr):
    """A decoded screen as one string, rows joined without their padding."""
    return '\n'.join(line.rstrip() for line in scr.lines())


PANIC_ENDINGS = ('Press a key to return to ', 'Press CTRL+ALT+DEL to reboot.', 'System halted.')
# The size UEFI Limine's fallback terminal gives the firmware's console.
FALLBACK_COLS = 80


def panic_message(scr, width=None):
    """The message of a panic shown in full on scr, or None. A message filling
    its row (the first width columns, if given) carries on in the next."""
    width = width or scr.cols
    rows = [row[:width] for row in scr.lines()]
    y = next((i for i, row in enumerate(rows) if 'PANIC: ' in row), None)
    if y is None or not any(e in row for row in rows[y + 1:] for e in PANIC_ENDINGS):
        return None
    msg = rows[y][rows[y].index('PANIC: ') + 7:]
    # A row wrapped at a space ends in it, one column short once stripped.
    while y + 1 < len(rows) and (len(rows[y].rstrip()) == width
                                 or (len(rows[y].rstrip()) == width - 1 and continues(rows[y + 1]))):
        y += 1
        msg += rows[y]
    return msg.rstrip()


def continues(row):
    """Whether a row below a panic message can be more of it."""
    return bool(row.strip()) and not row.startswith('Stacktrace:') and not any(e in row for e in PANIC_ENDINGS)


def shown_panic(t, m, **decode):
    """(message, decoded screen) of the panic a screenshot shows, the message
    None where it shows none. Before it has a terminal of its own, UEFI Limine
    prints through the firmware's console."""
    scr = None
    try:
        img = m.screendump('poll')
        scr = t.decode(m, img, **decode)
    except Exception:
        img = None
    msg = panic_message(scr) if scr is not None else None
    if msg is None and img is not None and t.port.is_uefi:
        fw = screen_mod.decode_efi_console(img, m.firmware)
        fw_msg = panic_message(fw, FALLBACK_COLS)
        if fw_msg is not None:
            return fw_msg, fw
    return msg, scr


def wait_panic(t, m, timeout=60, **decode):
    """Wait for a panic; return (message, decoded screen)."""
    deadline = time.monotonic() + timeout * t.port.timeout_scale
    while True:
        msg, scr = shown_panic(t, m, **decode)
        if msg is not None:
            # One taken while the screen is redrawn can hold rows half drawn,
            # so the message counts once the next screenshot shows it the same.
            time.sleep(SETTLE_INTERVAL)
            again, again_scr = shown_panic(t, m, **decode)
            if again == msg:
                return again, again_scr
        if m.exited() or time.monotonic() > deadline:
            t.fail('no panic; screen:\n' + (screen_text(scr) if scr is not None else '(none)'))
        time.sleep(0.5)


def panics_in_turn(t, m, count, timeout=60):
    """Boot each of the menu's first count entries in turn, a key taking the
    menu back after each panic; return the panic messages."""
    msgs = []
    for i in range(count):
        t.wait_text(m, 'ARROWS', timeout)
        m.press('home', *['down'] * i, delay=0.05)
        m.press('ret')
        msgs.append(wait_panic(t, m, timeout)[0])
        m.press('ret')
    return msgs


def wait_done_or_panic(t, m, channel, timeout=60):
    """Wait for a payload's "E DONE" on channel or a panic on screen; return
    ('done', None) or ('panic', message)."""
    deadline = time.monotonic() + timeout * t.port.timeout_scale
    while True:
        if 'E DONE' in m.stream(channel).text():
            return 'done', None
        try:
            scr = t.screen(m, 'poll')
        except Exception:
            scr = None
        msg = panic_message(scr) if scr is not None else None
        if msg is not None:
            return 'panic', msg
        text = screen_text(scr) if scr is not None else ''
        if m.exited() or time.monotonic() > deadline:
            t.fail(f'neither E DONE on {channel} nor a panic; screen:\n{text}')
        time.sleep(0.5)


def boot_entry(t, config, tree=None, files=None, kernel_variant='full', disk_kwargs=None, **machine):
    """Boot a disk holding a config, and the check executable as
    /boot/kernel.elf."""
    from harness.media import Tree
    tree = tree or Tree()
    if kernel_variant is not None:
        tree.add('/boot/kernel.elf', HostFile(kernel_file(t, kernel_variant)))
    for path, data in (files or {}).items():
        tree.add(path, data)
    t.limine_files(config, tree)
    disk = t.boot_disk(tree, **(disk_kwargs or {}))
    return t.boot(disk, **machine), disk
