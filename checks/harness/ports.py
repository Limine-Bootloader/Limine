import os
import re
import shutil
import subprocess
import tempfile

from harness.qemu import Machine


_qemu_versions = {}


def socket_dir():
    """A fresh directory for a machine's sockets, near enough the root for
    their paths to fit a socket address, 104 bytes on some systems."""
    base = tempfile.gettempdir()
    if len(base) > 64:
        base = '/tmp'
    return tempfile.mkdtemp(prefix='limine-check-', dir=base)


def qemu_version(qemu):
    """A QEMU executable's (major, minor) version."""
    if qemu not in _qemu_versions:
        out = subprocess.run([qemu, '--version'], stdout=subprocess.PIPE, universal_newlines=True).stdout
        m = re.search(r'version (\d+)\.(\d+)', out)
        _qemu_versions[qemu] = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
    return _qemu_versions[qemu]


class Port:
    def __init__(self, name, arch, firmware, qemu, efi_name=None, shell_name=None,
                 code=None, vars=None, memory='512M', machine='q35', cpu=None,
                 kernel_out='debugcon', limine_arch=None, timeout_scale=1.0):
        self.name = name
        self.arch = arch
        self.firmware = firmware
        self.qemu = qemu
        self.efi_name = efi_name
        self.shell_name = shell_name
        self.code = code
        self.vars = vars
        self.memory = memory
        self.machine = machine
        self.cpu = cpu
        # Where the payloads write: 'debugcon' (x86 port 0xe9) or 'serial'.
        self.kernel_out = kernel_out
        self.limine_arch = limine_arch or arch
        # LIMINE_CHECK_SLOWDOWN stretches every timeout, for slow hosts and
        # instrumented runs.
        self.timeout_scale = timeout_scale * float(os.environ.get('LIMINE_CHECK_SLOWDOWN', '1'))

    @property
    def is_uefi(self):
        return self.firmware == 'uefi'

    @property
    def is_x86(self):
        return self.arch in ('x86-64', 'ia-32')

    def __repr__(self):
        return f'Port({self.name})'


PORTS = {
    'bios': Port('bios', 'x86-64', 'bios', 'qemu-system-x86_64', machine='pc',
                 limine_arch='ia-32'),
    'uefi-ia32': Port('uefi-ia32', 'x86-64', 'uefi', 'qemu-system-x86_64',
                      efi_name='BOOTIA32.EFI', shell_name='shellia32.efi',
                      code='ovmf-code-ia32.fd', vars='ovmf-vars-ia32.fd', limine_arch='ia-32'),
    'uefi-x86-64': Port('uefi-x86-64', 'x86-64', 'uefi', 'qemu-system-x86_64',
                        efi_name='BOOTX64.EFI', shell_name='shellx64.efi',
                        code='ovmf-code-x86_64.fd', vars='ovmf-vars-x86_64.fd'),
    'uefi-aarch64': Port('uefi-aarch64', 'aarch64', 'uefi', 'qemu-system-aarch64',
                         efi_name='BOOTAA64.EFI', shell_name='shellaa64.efi',
                         code='ovmf-code-aarch64.fd', vars='ovmf-vars-aarch64.fd',
                         machine='virt', cpu='cortex-a72', kernel_out='serial',
                         timeout_scale=2.0),
    'uefi-riscv64': Port('uefi-riscv64', 'riscv64', 'uefi', 'qemu-system-riscv64',
                         efi_name='BOOTRISCV64.EFI', shell_name='shellriscv64.efi',
                         code='ovmf-code-riscv64.fd', vars='ovmf-vars-riscv64.fd',
                         machine='virt', cpu='rv64', kernel_out='serial', timeout_scale=2.0),
    'uefi-loongarch64': Port('uefi-loongarch64', 'loongarch64', 'uefi', 'qemu-system-loongarch64',
                             efi_name='BOOTLOONGARCH64.EFI', shell_name='shellloongarch64.efi',
                             code='ovmf-code-loongarch64.fd', vars='ovmf-vars-loongarch64.fd',
                             memory='1G', machine='virt', cpu='la464', kernel_out='serial',
                             timeout_scale=2.0),
}

UEFI_PORTS = [p for p in PORTS if PORTS[p].is_uefi]
ALL_PORTS = list(PORTS)
X86_PORTS = ['bios', 'uefi-ia32', 'uefi-x86-64']


class Drive:
    """A disk attached to the machine. kind: ide, ahci, virtio, nvme, usb,
    scsi, sd."""

    def __init__(self, path, kind=None, media='disk', readonly=False, bootindex=None,
                 sector_size=None, serial=None):
        self.path = path
        self.kind = kind
        self.media = media
        self.readonly = readonly
        self.bootindex = bootindex
        self.sector_size = sector_size
        self.serial = serial


class MachineSpec:
    """Everything a check may vary about the virtual machine."""

    def __init__(self, port, drives=None, memory=None, smp=1, cpu=None, machine=None,
                 machine_opts=None, display=True, display_dev=None, keyboard=True,
                 mouse=None, net=None, smbios=None, extra=None, no_reboot=True,
                 vars_file=None, code_file=None, tpm_socket=None, boot=None,
                 debugcon=True, rng=False):
        self.port = port
        self.drives = drives or []
        self.memory = memory or port.memory
        self.smp = smp
        self.cpu = cpu or port.cpu
        self.machine = machine or port.machine
        self.machine_opts = machine_opts or []
        self.display = display
        self.display_dev = display_dev
        self.keyboard = keyboard
        self.mouse = mouse
        self.net = net
        self.smbios = smbios or []
        self.extra = extra or []
        self.no_reboot = no_reboot
        self.vars_file = vars_file
        self.code_file = code_file
        self.tpm_socket = tpm_socket
        self.boot = boot
        self.debugcon = debugcon
        self.rng = rng


def _drive_args(port, idx, d):
    kind = d.kind
    if kind is None:
        if port.machine == 'pc':
            kind = 'ide'
        elif port.machine == 'q35':
            kind = 'ahci'
        else:
            kind = 'virtio'
    fmt = 'raw'
    drive_id = f'drv{idx}'
    opts = f'if=none,id={drive_id},format={fmt},file={d.path}'
    if d.readonly or d.media == 'cdrom':
        opts += ',readonly=on'
    args = ['-drive', opts]
    props = ''
    if d.bootindex is not None:
        props += f',bootindex={d.bootindex}'
    if d.sector_size is not None:
        props += f',logical_block_size={d.sector_size},physical_block_size={d.sector_size}'
    if d.serial is not None:
        props += f',serial={d.serial}'
    if kind in ('ide', 'ahci'):
        dev = 'ide-cd' if d.media == 'cdrom' else 'ide-hd'
        if kind == 'ide':
            args += ['-device', f'{dev},drive={drive_id},bus=ide.{idx // 2},unit={idx % 2}{props}']
        else:
            args += ['-device', f'{dev},drive={drive_id},bus=ide.{idx}{props}']
    elif kind == 'virtio':
        if d.media == 'cdrom':
            args += ['-device', f'virtio-scsi-pci,id=scsi{idx}',
                     '-device', f'scsi-cd,drive={drive_id},bus=scsi{idx}.0{props}']
        else:
            args += ['-device', f'virtio-blk-pci,drive={drive_id}{props}']
    elif kind == 'scsi':
        dev = 'scsi-cd' if d.media == 'cdrom' else 'scsi-hd'
        args += ['-device', f'virtio-scsi-pci,id=scsi{idx}',
                 '-device', f'{dev},drive={drive_id},bus=scsi{idx}.0{props}']
    elif kind == 'nvme':
        args += ['-device', f'nvme,drive={drive_id},serial=nvme{idx}{props}']
    elif kind == 'usb':
        args += ['-device', f'usb-storage,drive={drive_id}{props}']
    else:
        raise ValueError(f'unknown drive kind {kind}')
    return args, kind


def build_machine(env, spec, workdir, coverage=None):
    port = spec.port
    os.makedirs(workdir, exist_ok=True)
    version = qemu_version(port.qemu)
    argv = [port.qemu, '-S', '-nodefaults', '-no-user-config']
    # Dies with the harness, however that goes, rather than outliving it.
    if version >= (10, 2):
        argv += ['-run-with', 'exit-with-parent=on']
    machine = spec.machine
    if spec.machine_opts:
        machine += ',' + ','.join(spec.machine_opts)
    argv += ['-M', machine, '-m', spec.memory, '-smp', str(spec.smp)]
    if spec.cpu:
        argv += ['-cpu', spec.cpu]
    if spec.no_reboot:
        argv += ['-no-reboot']
    argv += ['-display', 'none']
    sockdir = socket_dir()

    def quoted(path):
        # QEMU takes a doubled comma for one in an option's value.
        return path.replace(',', ',,')

    # The guest's output goes to a file, which takes it as fast as it comes:
    # a UART QEMU keeps busy while a reader catches up makes firmware drop
    # bytes. Input comes from a FIFO.
    def chardev(name):
        return (f'file,id={name},path={quoted(os.path.join(workdir, name + ".log"))},'
                f'input-path={quoted(os.path.join(sockdir, name + ".in"))}')
    argv += ['-chardev', f'socket,id=qmp,path={quoted(os.path.join(sockdir, "qmp"))},server=on,wait=off']
    # QEMU 11.1 has monitors made as objects, and deprecates -mon.
    if version >= (11, 1):
        argv += ['-object', 'monitor-qmp,id=qmpmon,chardev=qmp']
    else:
        argv += ['-mon', 'chardev=qmp,mode=control']
    chardevs = ['serial']
    argv += ['-chardev', chardev('serial'), '-serial', 'chardev:serial']
    if port.is_x86 and spec.debugcon:
        chardevs.append('debugcon')
        argv += ['-chardev', chardev('debugcon'), '-device', 'isa-debugcon,iobase=0xe9,chardev=debugcon']
    if port.is_uefi:
        code = spec.code_file or env.ovmf(port.code)
        vars_path = os.path.join(workdir, 'vars.fd')
        if not os.path.exists(vars_path):
            shutil.copyfile(spec.vars_file or env.ovmf(port.vars), vars_path)
        argv += ['-drive', f'if=pflash,unit=0,format=raw,file={code},readonly=on',
                 '-drive', f'if=pflash,unit=1,format=raw,file={vars_path}']
    usb = False
    if port.machine == 'virt':
        usb = True
    if spec.display:
        dev = spec.display_dev
        if dev is None:
            dev = 'VGA' if port.is_x86 else 'ramfb'
        if dev:
            argv += ['-device', dev]
    if spec.keyboard:
        if port.machine == 'virt':
            argv += ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0']
        # pc and q35 bring their PS/2 keyboard and mouse along.
    if spec.mouse:
        if spec.mouse in ('usb-mouse', 'usb-tablet'):
            if port.machine != 'virt':
                argv += ['-device', 'qemu-xhci,id=xhci']
            argv += ['-device', f'{spec.mouse},bus=xhci.0']
        elif spec.mouse != 'ps2':
            argv += ['-device', spec.mouse]
    for idx, d in enumerate(spec.drives):
        dargs, kind = _drive_args(port, idx, d)
        if kind == 'usb' and not usb and '-device' in argv and 'qemu-xhci,id=xhci' not in argv:
            argv += ['-device', 'qemu-xhci,id=xhci']
        argv += dargs
    if spec.net is not None:
        argv += spec.net
    else:
        argv += ['-net', 'none']
    for s in spec.smbios:
        argv += ['-smbios', s]
    if spec.tpm_socket is not None:
        argv += ['-chardev', f'socket,id=chrtpm,path={spec.tpm_socket}',
                 '-tpmdev', 'emulator,id=tpm0,chardev=chrtpm']
        if port.is_x86:
            argv += ['-device', 'tpm-tis,tpmdev=tpm0']
        else:
            argv += ['-device', 'tpm-tis-device,tpmdev=tpm0']
    if spec.rng:
        argv += ['-object', 'rng-random,filename=/dev/urandom,id=rng0',
                 '-device', 'virtio-rng-pci,rng=rng0']
    if spec.boot is not None:
        argv += ['-boot', spec.boot]
    if coverage is not None:
        argv += coverage.qemu_args(port, workdir)
    argv += spec.extra
    m = Machine(workdir, argv, chardevs, sockdir, timeout_scale=port.timeout_scale,
                nudge=spec.tpm_socket is not None)
    m.firmware = code if port.is_uefi else None
    return m
