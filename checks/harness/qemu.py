import json
import os
import re
import shutil
import signal
import socket
import subprocess
import threading
import time

from harness import screen as screen_mod


class CheckTimeout(Exception):
    pass


class Stream:
    """Accumulates everything a character device sends, for pattern waits:
    the file QEMU writes it to, read as it grows. What is sent to the device
    goes through the FIFO QEMU reads its input from."""

    def __init__(self, name, path, fifo, exited):
        self.name = name
        self.fd = os.open(path, os.O_RDONLY)
        self.fifo = fifo
        self.exited = exited
        self.data = bytearray()
        self.cond = threading.Condition()
        self.eof = False
        self.closing = threading.Event()
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _drain(self):
        """Take in all QEMU has written so far, with the lock held: what
        callers see then includes all it wrote before what they waited for
        on another device."""
        while self.fd is not None:
            chunk = os.read(self.fd, 65536)
            if not chunk:
                return
            self.data += chunk

    def _reader(self):
        """Wakes waiters on new output, and marks the end once QEMU exited."""
        while True:
            # Asked before reading, so that what QEMU wrote before it exited
            # is all read.
            done = self.exited() or self.closing.is_set()
            with self.cond:
                size = len(self.data)
                self._drain()
                if len(self.data) != size:
                    self.cond.notify_all()
                elif done:
                    self.eof = True
                    self.cond.notify_all()
                    return
            self.closing.wait(0.01)

    def write(self, data):
        if isinstance(data, str):
            data = data.encode()
        while data:
            data = data[os.write(self.fifo, data):]

    def raw(self):
        with self.cond:
            self._drain()
            return bytes(self.data)

    def text(self):
        with self.cond:
            self._drain()
            return self.data.decode('latin-1')

    def size(self):
        with self.cond:
            self._drain()
            return len(self.data)

    def wait_for(self, pattern, timeout, start=0, machine=None):
        """Wait until pattern (bytes, str or compiled regex) appears at or after
        start. Returns the match end offset (or the match object for
        regexes)."""
        if isinstance(pattern, str):
            pattern = pattern.encode('latin-1')
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                exited = machine is not None and machine.exited()
                self._drain()
                if isinstance(pattern, bytes):
                    idx = self.data.find(pattern, start)
                    if idx >= 0:
                        return idx + len(pattern)
                else:
                    m = pattern.search(bytes(self.data), start)
                    if m:
                        return m
                if self.eof:
                    raise CheckTimeout(f'{self.name}: stream closed while waiting for {pattern!r}')
                if exited:
                    raise CheckTimeout(f'{self.name}: QEMU exited while waiting for {pattern!r}')
                left = deadline - time.monotonic()
                if left <= 0:
                    raise CheckTimeout(f'{self.name}: timed out after {timeout}s waiting for {pattern!r}')
                self.cond.wait(min(left, 0.5))

    def close(self):
        if self.closing.is_set():
            return
        self.closing.set()
        self.thread.join(timeout=5)
        with self.cond:
            self._drain()
            os.close(self.fd)
            self.fd = None
        os.close(self.fifo)


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(path)
        self.file = self.sock.makefile('rwb')
        self.events = []
        self.lock = threading.Lock()
        greeting = self._read()
        if 'QMP' not in greeting:
            raise RuntimeError(f'unexpected QMP greeting: {greeting}')
        self.cmd('qmp_capabilities')

    def _read(self):
        line = self.file.readline()
        if not line:
            raise ConnectionError('QMP connection closed')
        return json.loads(line)

    def cmd(self, name, **args):
        with self.lock:
            msg = {'execute': name}
            if args:
                msg['arguments'] = args
            self.file.write(json.dumps(msg).encode() + b'\n')
            self.file.flush()
            while True:
                r = self._read()
                if 'event' in r:
                    self.events.append(r)
                    continue
                if 'error' in r:
                    raise RuntimeError(f'QMP {name} failed: {r["error"]}')
                return r.get('return')

    def close(self):
        try:
            self.file.close()
            self.sock.close()
        except OSError:
            pass


# Keys as QMP qcodes. Characters needing shift map to (qcode, True).
_QCODES = {
    ' ': 'spc', '\n': 'ret', '\t': 'tab', '\b': 'backspace', '\x1b': 'esc',
    '-': 'minus', '=': 'equal', '[': 'bracket_left', ']': 'bracket_right',
    ';': 'semicolon', "'": 'apostrophe', '`': 'grave_accent', '\\': 'backslash',
    ',': 'comma', '.': 'dot', '/': 'slash',
}
_SHIFTED = {
    '!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
    '(': '9', ')': '0', '_': 'minus', '+': 'equal', '{': 'bracket_left',
    '}': 'bracket_right', ':': 'semicolon', '"': 'apostrophe', '~': 'grave_accent',
    '|': 'backslash', '<': 'comma', '>': 'dot', '?': 'slash',
}


def char_qcode(c):
    if c.isalpha() and c.isascii():
        return c.lower(), c.isupper()
    if c.isdigit():
        return c, False
    if c in _QCODES:
        return _QCODES[c], False
    if c in _SHIFTED:
        base = _SHIFTED[c]
        return _QCODES.get(base, base), True
    raise ValueError(f'no qcode for {c!r}')


class Machine:
    """One QEMU instance, paused at creation until start() connects it all."""

    def __init__(self, workdir, argv, chardevs, sockdir, timeout_scale=1.0, coverage_out=None,
                 nudge=False):
        self.workdir = workdir
        self.argv = argv
        self.chardev_names = chardevs
        self.sockdir = sockdir
        self.proc = None
        self.qmp = None
        self.streams = {}
        self.timeout_scale = timeout_scale
        self.coverage_out = coverage_out
        self.shot_counter = 0
        self.nudge = nudge
        self.nudge_stop = threading.Event()
        self.nudge_thread = None

    def start(self):
        os.makedirs(self.workdir, exist_ok=True)
        with open(os.path.join(self.workdir, 'qemu-cmdline.txt'), 'w') as f:
            f.write(' '.join(shlex_quote(a) for a in self.argv) + '\n')
        self.stderr = open(os.path.join(self.workdir, 'qemu-stderr.log'), 'wb')
        for name in self.chardev_names:
            fifo = self._addr(name + '.in')
            os.mkfifo(fifo)
            # QEMU opening its input to read waits for a writer, as opening it
            # to write would wait for a reader: this end, opened for both first.
            fd = os.open(fifo, os.O_RDWR)
            log = os.path.join(self.workdir, f'{name}.log')
            open(log, 'wb').close()
            self.streams[name] = Stream(name, log, fd, self.exited)
        self.proc = subprocess.Popen(self.argv, stdin=subprocess.DEVNULL, stdout=self.stderr,
                                     stderr=subprocess.STDOUT, cwd=self.workdir,
                                     start_new_session=True)
        deadline = time.monotonic() + 30
        while True:
            try:
                self.qmp = Qmp(self._addr('qmp'))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if self.proc.poll() is not None:
                    raise RuntimeError(f'QEMU failed to start:\n{self._stderr_text()}')
                if time.monotonic() > deadline:
                    raise RuntimeError('the QMP socket did not appear')
                time.sleep(0.02)
        self.qmp.cmd('cont')
        if self.nudge:
            self.nudge_thread = threading.Thread(target=self._nudge, daemon=True)
            self.nudge_thread.start()
        return self

    def _nudge(self):
        # QEMU 11.1.2 now and then sits on a TPM response, leaving the guest
        # polling for it forever, until something wakes it; a QMP command does.
        while not self.nudge_stop.wait(0.5):
            try:
                self.qmp.cmd('query-status')
            except Exception:
                return

    def _addr(self, name):
        return os.path.join(self.sockdir, name)

    def _stderr_text(self):
        try:
            with open(os.path.join(self.workdir, 'qemu-stderr.log'), 'rb') as f:
                return f.read().decode(errors='replace')
        except OSError:
            return ''

    def exited(self):
        return self.proc is not None and self.proc.poll() is not None

    def stream(self, name):
        return self.streams[name]

    def wait_for(self, name, pattern, timeout=30, start=0):
        return self.streams[name].wait_for(pattern, timeout * self.timeout_scale, start, self)

    def wait_exit(self, timeout=30):
        try:
            return self.proc.wait(timeout * self.timeout_scale)
        except subprocess.TimeoutExpired:
            raise CheckTimeout(f'QEMU did not exit within {timeout}s')

    def send_key(self, *keys, hold_ms=None):
        """Press keys (qcodes) together, e.g. send_key('ctrl', 'x')."""
        args = {'keys': [{'type': 'qcode', 'data': k} for k in keys]}
        if hold_ms is not None:
            args['hold-time'] = hold_ms
        self.qmp.cmd('send-key', **args)

    def press(self, *seq, delay=0.15):
        """Press keys one after another; each item is a qcode or a tuple."""
        for k in seq:
            if isinstance(k, (tuple, list)):
                self.send_key(*k)
            else:
                self.send_key(k)
            time.sleep(delay)

    def type_text(self, text, delay=0.08):
        for c in text:
            code, shift = char_qcode(c)
            if shift:
                self.send_key('shift', code)
            else:
                self.send_key(code)
            time.sleep(delay)

    def mouse_move(self, dx, dy):
        self.qmp.cmd('input-send-event', events=[
            {'type': 'rel', 'data': {'axis': 'x', 'value': dx}},
            {'type': 'rel', 'data': {'axis': 'y', 'value': dy}},
        ])

    def mouse_abs(self, x, y):
        """Absolute pointer position, 0..0x7fff on each axis (tablets)."""
        self.qmp.cmd('input-send-event', events=[
            {'type': 'abs', 'data': {'axis': 'x', 'value': x}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': y}},
        ])

    def mouse_button(self, button='left', down=True):
        self.qmp.cmd('input-send-event', events=[
            {'type': 'btn', 'data': {'down': down, 'button': button}},
        ])

    def mouse_click(self, button='left'):
        # Held long enough for the press to be seen by firmware that polls the
        # device and reports only the button state it then finds.
        self.mouse_button(button, True)
        time.sleep(0.2)
        self.mouse_button(button, False)

    def mouse_wheel(self, steps):
        button = 'wheel-down' if steps > 0 else 'wheel-up'
        for _ in range(abs(steps)):
            self.mouse_click(button)
            time.sleep(0.05)

    def screendump(self, name=None, head=None, device=None):
        self.shot_counter += 1
        name = name or f'screen{self.shot_counter:03d}'
        path = os.path.join(self.workdir, name + '.ppm')
        args = {'filename': path}
        if device is not None:
            args['device'] = device
        if head is not None:
            args['head'] = head
        self.qmp.cmd('screendump', **args)
        return screen_mod.Image.load_ppm(path)

    def read_phys(self, addr, size):
        path = os.path.join(self.workdir, 'pmem.bin')
        self.qmp.cmd('pmemsave', val=addr, size=size, filename=path)
        with open(path, 'rb') as f:
            data = f.read()
        os.unlink(path)
        return data

    def hmp(self, command):
        return self.qmp.cmd('human-monitor-command', **{'command-line': command})

    def stop(self):
        if self.proc is not None:
            self._stop_qemu()
        for s in self.streams.values():
            s.close()
        shutil.rmtree(self.sockdir, ignore_errors=True)

    def _stop_qemu(self):
        self.nudge_stop.set()
        if self.nudge_thread is not None:
            self.nudge_thread.join(5)
        if self.proc.poll() is None:
            try:
                self.qmp.cmd('quit')
            except Exception:
                pass
            try:
                self.proc.wait(20)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait()
        if self.qmp is not None:
            self.qmp.close()
        self.stderr.close()

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()


def shlex_quote(s):
    if re.fullmatch(r'[A-Za-z0-9_@%+=:,./-]+', s):
        return s
    return "'" + s.replace("'", "'\\''") + "'"


def copy_vars(src, dest):
    shutil.copyfile(src, dest)
    return dest
