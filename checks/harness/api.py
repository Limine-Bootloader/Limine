import os
import time
import traceback

from harness import media
from harness.media import Disk, Part, Fat, Iso, HostFile, Tree, ESP_TYPE
from harness.ports import PORTS, MachineSpec, Drive, build_machine, qemu_version
from harness.report import Report
from harness.tpm import SoftwareTpm
from harness.bootserver import BootServer
from harness.qemu import CheckTimeout
from harness import screen as screen_mod
from harness import vt as vt_mod

REGISTRY = []


EFI_NAMES = ['BOOTX64.EFI', 'BOOTIA32.EFI', 'BOOTAA64.EFI', 'BOOTRISCV64.EFI', 'BOOTLOONGARCH64.EFI']

# Seconds between a screenshot that shows what is waited for and the one that
# confirms it.
SETTLE_INTERVAL = 0.1


class CheckFailure(AssertionError):
    pass


class Skip(Exception):
    pass


class CheckDef:
    def __init__(self, func, ports, tags, timeout, name):
        self.func = func
        self.ports = ports
        self.tags = tags
        self.timeout = timeout
        self.name = name
        self.module = func.__module__.rsplit('.', 1)[-1]

    @property
    def full_name(self):
        return f'{self.module}.{self.name}'


def check(ports=None, tags=(), timeout=180, name=None):
    """Register a check function, run once per port in ports."""
    def wrap(func):
        REGISTRY.append(CheckDef(func, list(ports or PORTS), list(tags), timeout, name or func.__name__))
        return func
    return wrap


def payload_arch(port):
    return {'x86-64': 'x86_64', 'ia-32': 'x86_64'}.get(port.arch, port.arch)


class Context:
    """Everything one check run gets: its port, a work directory, and
    helpers that build media, start machines and record failures."""

    def __init__(self, env, port, workdir, coverage=None):
        self.env = env
        self.port = port
        self.workdir = workdir
        self.coverage = coverage
        self.machines = []
        self.tpms = []
        self.servers = []
        self.soft_failures = []
        self.notes = []
        self.counter = 0
        os.makedirs(workdir, exist_ok=True)

    # Results

    def fail(self, msg):
        raise CheckFailure(msg)

    def check(self, cond, msg):
        """A soft assertion: recorded, and the check carries on."""
        if not cond:
            self.soft_failures.append(msg)
        return cond

    def expect_eq(self, actual, expected, what):
        return self.check(actual == expected, f'{what}: got {actual!r}, expected {expected!r}')

    def require(self, cond, msg):
        if not cond:
            raise CheckFailure(msg)

    def note(self, msg):
        self.notes.append(msg)

    def skip(self, why):
        raise Skip(why)

    def skip_unless_built(self, *names):
        """Skip unless the build made each of the files names, as the CD and
        PXE images are only made when configured."""
        for name in names:
            if not os.path.exists(self.env.bin(name)):
                self.skip(f'the build made no {name}')

    def path(self, name):
        return os.path.join(self.workdir, name)

    # Media

    def limine_files(self, config=None, tree=None, conf_path='/boot/limine.conf',
                     bios_sys_path='/boot/limine-bios.sys', efi_path=None):
        """A tree with what booting this port from a disk needs, plus config."""
        tree = tree or Tree()
        if self.port.is_uefi:
            tree.add(efi_path or ('/EFI/BOOT/' + self.port.efi_name), HostFile(self.env.bin(self.port.efi_name)))
        elif bios_sys_path is not None:
            tree.add(bios_sys_path, HostFile(self.env.bin('limine-bios.sys')))
        if config is not None and conf_path is not None:
            tree.add(conf_path, config)
        return tree

    def boot_disk(self, tree, scheme=None, fat=32, size_mb=64, name=None, install=True,
                  label='LIMINE', volid=None, extra_parts=None, disk_guid=None, mbr_id=None,
                  part_guid=None, sector_size=512, cluster_sectors=None):
        """A disk with one FAT partition holding tree, bootable for the port."""
        if scheme is None:
            scheme = 'gpt' if self.port.is_uefi else 'mbr'
        self.counter += 1
        name = name or f'disk{self.counter}.img'
        path = self.path(name)
        fs = Fat(fat, label=label, volid=volid, files=tree, sector_size=sector_size,
                 cluster_sectors=cluster_sectors)
        if scheme == 'none':
            disk = Disk('none', size_mb, fs=fs, sector_size=sector_size)
        else:
            ptype = ESP_TYPE if scheme == 'gpt' else ('0c' if fat == 32 else '0e' if fat == 16 else '01')
            # With partitions to follow, the boot one leaves them room.
            boot_size = None
            if extra_parts:
                boot_size = size_mb - 2 - sum(p.size_mb or 0 for p in extra_parts)
            parts = [Part(fs=fs, type=ptype, guid=part_guid, bootable=scheme == 'mbr', size_mb=boot_size)]
            parts += extra_parts or []
            disk = Disk(scheme, size_mb, parts=parts, disk_guid=disk_guid, mbr_id=mbr_id,
                        sector_size=sector_size)
        disk.build(path, self.workdir)
        if install and not self.port.is_uefi:
            media.bios_install(self.env, path)
        disk.path = path
        return disk

    def iso(self, tree, name=None, hybrid=True, bios=None, uefi=None, **kwargs):
        """An ISO image. A hybrid one is made the way USAGE.md says: both El
        Torito images, the EFI executables, a protective MBR, then
        bios-install. Otherwise a plain CD booting the port through El
        Torito."""
        self.counter += 1
        name = name or f'cd{self.counter}.iso'
        if hybrid:
            self.skip_unless_built('limine-uefi-cd.bin' if self.port.is_uefi else 'limine-bios-cd.bin')
            # As much of it as the ports the build has make.
            bios = os.path.exists(self.env.bin('limine-bios-cd.bin'))
            uefi = os.path.exists(self.env.bin('limine-uefi-cd.bin'))
            if bios:
                tree.add('/boot/limine-bios.sys', HostFile(self.env.bin('limine-bios.sys')))
            for efi in EFI_NAMES:
                if os.path.exists(self.env.bin(efi)):
                    tree.add('/EFI/BOOT/' + efi, HostFile(self.env.bin(efi)))
        else:
            bios = (not self.port.is_uefi) if bios is None else bios
            uefi = self.port.is_uefi if uefi is None else uefi
            if bios:
                self.skip_unless_built('limine-bios-cd.bin')
            if uefi:
                self.skip_unless_built('limine-uefi-cd.bin')
        if bios:
            tree.add('/boot/limine-bios-cd.bin', HostFile(self.env.bin('limine-bios-cd.bin')))
        if uefi:
            tree.add('/boot/limine-uefi-cd.bin', HostFile(self.env.bin('limine-uefi-cd.bin')))
        iso = Iso(tree, bios_boot='/boot/limine-bios-cd.bin' if bios else None,
                  uefi_boot='/boot/limine-uefi-cd.bin' if uefi else None, hybrid=hybrid, **kwargs)
        path = self.path(name)
        iso.build(path, self.workdir)
        if hybrid and bios:
            media.bios_install(self.env, path)
        iso.path = path
        return iso

    # Machines

    def software_tpm(self):
        """A TPM 2.0 for machines of this check to use, stopped with them."""
        if self.port.name == 'uefi-loongarch64' and qemu_version(self.port.qemu) < (10, 0):
            self.skip('QEMU before 10.0 shows loongarch64 firmware no TPM')
        tpm = SoftwareTpm(self.workdir)
        self.tpms.append(tpm)
        return tpm

    def machine(self, drives=(), start=True, name=None, tpm=None, **kwargs):
        self.counter += 1
        mdir = self.path(name or f'vm{self.counter}')
        if tpm is not None:
            kwargs['tpm_socket'] = tpm.socket(mdir)
        spec = MachineSpec(self.port, drives=list(drives), **kwargs)
        m = build_machine(self.env, spec, mdir, coverage=self.coverage)
        self.machines.append(m)
        if start:
            m.start()
        return m

    def boot(self, disk=None, cdrom=None, drives=None, **kwargs):
        """Start a machine booting from disk (or cdrom) and return it."""
        if drives is None:
            drives = []
            if disk is not None:
                drives.append(Drive(disk.path if hasattr(disk, 'path') else disk, bootindex=1))
            if cdrom is not None:
                drives.append(Drive(cdrom.path if hasattr(cdrom, 'path') else cdrom,
                                    media='cdrom', bootindex=0 if disk is None else 2))
        return self.machine(drives=drives, **kwargs)

    def boot_pxe(self, tree, bootfile, tftp_options=None, bootfile_size=False, **kwargs):
        """Start a machine booting bootfile over PXE, from the DHCP and TFTP
        servers of QEMU's user networking, the latter serving tree; or, given
        the TFTP options to support, from a BootServer, as the machine's
        boot_server."""
        if not self.port.is_uefi:
            self.skip_unless_built('limine-bios-pxe.bin')
        self.counter += 1
        root = self.path(f'tftp{self.counter}')
        tree.materialise(root)
        server = None
        if tftp_options is None:
            netdev = f'user,id=net0,tftp={root},bootfile={bootfile}'
        else:
            server = BootServer(root, bootfile, tftp_options, bootfile_size)
            self.servers.append(server)
            netdev = server.netdev('net0')
        if self.port.is_uefi:
            # edk2's own driver, not the iPXE one QEMU's option ROM carries.
            # No boot order: given one, edk2 off x86 makes its boot options
            # without dispatching the network stack first, which waits for an
            # RNG, here a virtio one, as TCG's CPUs have none.
            nic = 'virtio-net-pci,netdev=net0,romfile='
            kwargs.setdefault('rng', True)
        else:
            nic = 'virtio-net-pci,netdev=net0,bootindex=1'
        m = self.machine(net=['-netdev', netdev, '-device', nic], **kwargs)
        m.boot_server = server
        return m

    def stop_all(self):
        for m in self.machines:
            try:
                m.stop()
            except Exception:
                traceback.print_exc()
        self.machines = []
        for tpm in self.tpms:
            tpm.stop()
        self.tpms = []
        for server in self.servers:
            server.stop()
        self.servers = []

    # Observation

    @property
    def kernel_channel(self):
        return self.port.kernel_out

    def kernel_report(self, m, timeout=60, begin='B LIMINE-TEST'):
        """Wait for a check payload to finish and parse what it printed."""
        ch = self.kernel_channel
        m.wait_for(ch, 'E DONE', timeout)
        # The rest of the line.
        m.wait_for(ch, '\n', 5, start=m.stream(ch).text().rfind('E DONE'))
        return Report(m.stream(ch).text(), begin)

    def decode(self, m, img, rotate=0, **kwargs):
        """The text on screen: VGA text memory when the adapter shows 80x25
        text (720x400), Limine's graphical terminal otherwise, read after
        turning the picture rotate degrees clockwise."""
        if not self.port.is_uefi and (img.width, img.height) == (720, 400):
            return screen_mod.decode_vga_text(m.read_phys(0xb8000, 80 * 25 * 2))
        return screen_mod.decode_gterm(img.rotate(rotate), **kwargs)

    def screen(self, m, name=None, **kwargs):
        return self.decode(m, m.screendump(name), **kwargs)

    def firmware_screen(self, m, name=None):
        """The text edk2's own console shows, which UEFI Limine prints through
        before, or instead of, a terminal of its own."""
        return screen_mod.decode_efi_console(m.screendump(name), m.firmware)

    def wait_screen(self, m, predicate, timeout=30, interval=0.5, desc='screen condition',
                    firmware=False, **kwargs):
        """Poll screenshots until predicate(text_screen) holds. firmware reads
        them as edk2's console rather than Limine's terminal."""
        def poll():
            img = m.screendump('poll')
            try:
                if firmware:
                    return screen_mod.decode_efi_console(img, m.firmware)
                return self.decode(m, img, **kwargs)
            except Exception:
                return None

        deadline = time.monotonic() + timeout * self.port.timeout_scale
        while True:
            last = poll()
            if last is not None and predicate(last):
                # One taken while the screen is redrawn can hold rows half
                # drawn, so the screen counts once the next reads the same.
                time.sleep(SETTLE_INTERVAL)
                again = poll()
                if again is not None and again.text() == last.text():
                    return again
            if m.exited():
                raise CheckTimeout(f'QEMU exited while waiting for {desc}')
            if time.monotonic() > deadline:
                if last is not None:
                    raise CheckTimeout(f'timed out waiting for {desc}; last screen:\n{last.text()}')
                raise CheckTimeout(f'timed out waiting for {desc}')
            time.sleep(interval)

    def wait_text(self, m, needle, timeout=30, **kwargs):
        return self.wait_screen(m, lambda s: s.find(needle), timeout, desc=repr(needle), **kwargs)

    def serial_terminal(self, m, cols=200, rows=60):
        term = vt_mod.Terminal(cols, rows)
        term.feed(m.stream('serial').raw())
        return term

    def wait_serial_text(self, m, needle, timeout=30, cols=80, rows=25):
        """Poll the screen the serial line draws until needle is on it."""
        deadline = time.monotonic() + timeout * self.port.timeout_scale
        while True:
            scr = self.serial_terminal(m, cols, rows).screen()
            if scr.find(needle):
                return scr
            if m.exited():
                raise CheckTimeout(f'QEMU exited while waiting for {needle!r} on the serial line')
            if time.monotonic() > deadline:
                raise CheckTimeout(f'timed out waiting for {needle!r} on the serial line; last screen:\n'
                                  + scr.text())
            time.sleep(0.5)

    def serial_plain(self, m):
        """The text written to the serial line in the order it was written,
        whatever escape sequences placed it."""
        return vt_mod.plain(m.stream('serial').raw())
