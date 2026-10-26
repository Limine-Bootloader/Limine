import os
import shutil

SRCDIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHECKSDIR = os.path.join(SRCDIR, 'checks')


class Env:
    """Where everything a run needs is, settled once from the command line."""

    def __init__(self, builddir=None, outdir=None, ovmfdir=None):
        self.srcdir = SRCDIR
        self.builddir = os.path.abspath(builddir or SRCDIR)
        self.bindir = os.path.join(self.builddir, 'bin')
        self.payloaddir = os.path.join(self.builddir, 'checks-build')
        self.outdir = os.path.abspath(outdir or os.path.join(self.builddir, 'checks-results'))
        self.ovmfdir = os.path.abspath(ovmfdir or os.path.join(self.builddir, 'edk2-ovmf-bins'))
        # An instrumented build of the host tool, used in its place when set.
        self.host_tool = None

    def bin(self, name):
        if name == 'limine' and self.host_tool is not None:
            return self.host_tool
        return os.path.join(self.bindir, name)

    def payload(self, *parts):
        return os.path.join(self.payloaddir, *parts)

    def ovmf(self, name):
        return os.path.join(self.ovmfdir, name)

    def limine_elf(self, port):
        sub = {
            'bios': 'common-bios',
            'uefi-ia32': 'common-uefi-ia32',
            'uefi-x86-64': 'common-uefi-x86-64',
            'uefi-aarch64': 'common-uefi-aarch64',
            'uefi-riscv64': 'common-uefi-riscv64',
            'uefi-loongarch64': 'common-uefi-loongarch64',
        }[port]
        return os.path.join(self.builddir, sub, 'limine.elf')


def which(prog):
    path = shutil.which(prog)
    if path is None:
        raise RuntimeError(f'required program not found: {prog}')
    return path
