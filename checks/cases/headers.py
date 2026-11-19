import os
import subprocess

from harness.api import check
from harness.env import SRCDIR
from cases.tsc import build_cc


def efi_build_cc(t):
    cc = build_cc(t)
    macros = subprocess.check_output(cc + ['-dM', '-E', '-x', 'c', '-'], input=b'')
    if b'#define __i386__ 1' in macros.splitlines():
        cc.append('-malign-double')
    return cc


@check(ports=['bios'], tags=['ci', 'tpm'])
def tpm_log_memory(t):
    """Each log is bounded by its own firmware RAM descriptor."""
    exe = t.path('tpm_mem')
    command = efi_build_cc(t) + ['-std=gnu11', '-Wall', '-Wextra', '-Werror',
                             '-I', os.path.join(SRCDIR, 'picoefi', 'include'),
                             '-I', os.path.join(SRCDIR, 'common'),
                             os.path.join(SRCDIR, 'checks', 'sim', 'tpm_mem.c'), '-o', exe]
    subprocess.run(command, check=True)
    subprocess.run([exe], check=True)
