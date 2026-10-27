"""A TPM 2.0 for the checks' machines, emulated by swtpm."""

import os
import subprocess
import time
import uuid

from harness.env import which
from harness.screen import efi_volumes

TPM2_FILE = uuid.UUID('fdff263d-5f68-4591-87ba-b768f445a9af').bytes_le
_supports = {}


def firmware_supports_tpm(code):
    """Whether an edk2 image holds the firmware file that gives it TPM 2.0
    support, without which no TPM is visible to what it boots."""
    if code not in _supports:
        with open(code, 'rb') as f:
            blob = f.read()
        _supports[code] = any(TPM2_FILE in vol for vol in efi_volumes(blob))
    return _supports[code]


class SoftwareTpm:
    """swtpm serving the machines of one check, one at a time; each power on
    resets its PCRs. Its socket is named relative to the directories swtpm
    and the machines run in, as a full path can outgrow a socket address."""

    def __init__(self, workdir):
        self.dir = os.path.join(workdir, 'tpm')
        os.makedirs(self.dir, exist_ok=True)
        self.log = open(os.path.join(self.dir, 'swtpm.log'), 'wb')
        self.proc = subprocess.Popen([which('swtpm'), 'socket', '--tpm2', '--tpmstate', 'dir=.',
                                      '--ctrl', 'type=unixio,path=swtpm.sock'],
                                     cwd=self.dir, stdin=subprocess.DEVNULL, stdout=self.log,
                                     stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 10
        while not os.path.exists(os.path.join(self.dir, 'swtpm.sock')):
            if self.proc.poll() is not None or time.monotonic() > deadline:
                self.stop()
                with open(os.path.join(self.dir, 'swtpm.log'), errors='replace') as f:
                    raise RuntimeError(f'swtpm did not start:\n{f.read()}')
            time.sleep(0.02)

    def socket(self, machine_dir):
        """The socket's name as seen from a machine's directory."""
        return os.path.relpath(os.path.join(self.dir, 'swtpm.sock'), machine_dir)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.log.close()
