from harness import fdt
from harness.api import check
from harness.media import HostFile
from harness.ports import ALL_PORTS
from cases._common import boot_entry, check_report_clean, entry, kernel_file, panics_in_turn
from cases.limine_protocol import boot_variant

# Device trees given in the configuration (CONFIG.md's dtb_path and
# global_dtb) or by the firmware when there is no ACPI, as the Limine
# protocol's DTB feature passes them on (PROTOCOL.md: memory nodes removed)
# and as Limine itself reads them for the CPUs to start.

PSCI_CPU_ON_64 = 0xc4000003
# What QEMU's rv64 harts have, as QEMU's own device tree lists it.
RV_EXTENSIONS = ['i', 'm', 'a', 'f', 'd', 'c', 'h', 'zicbom', 'zicboz', 'zicntr', 'zicsr', 'zifencei', 'zihpm',
                 'sstc', 'svadu']
RV_ISA = 'rv64imafdch_zicbom_zicboz_zicntr_zicsr_zifencei_zihpm_sstc_svadu'


def riscv_hart(hartid, mmu='riscv,sv57', extensions=RV_EXTENSIONS, isa=None, base='rv64i', **props):
    node = {'device_type': 'cpu', 'reg': hartid, 'status': 'okay', 'compatible': 'riscv'}
    if mmu is not None:
        node['mmu-type'] = mmu
    if extensions is not None:
        node['riscv,isa-extensions'] = extensions
        node['riscv,isa-base'] = base
    if isa is not None:
        node['riscv,isa'] = isa
    node.update(props)
    return node


def riscv_cpus(harts, timebase=(10000000,)):
    cpus = {'#address-cells': 1, '#size-cells': 0}
    if timebase is not None:
        cpus['timebase-frequency'] = timebase
    for hartid, node in harts:
        cpus[f'cpu@{hartid:x}'] = node
    return cpus


def tree(t, marker, cpus=None, **extra):
    """A device tree with a marker, two memory nodes for the bootloader to
    remove, and, on RISC-V, where Limine takes the harts from it, the harts
    the machine has unless cpus says otherwise (False: no /cpus)."""
    root = {'#address-cells': 2, '#size-cells': 2, 'compatible': 'limine,test', 'model': 'limine test tree',
            'limine-test-marker': marker,
            'memory@40000000': {'device_type': 'memory', 'reg': (0, 0x40000000, 0, 0x10000000)},
            'memory@80000000': {'device_type': 'memory', 'reg': (0, 0x80000000, 0, 0x10000000)}}
    if cpus is None and t.port.arch == 'riscv64':
        cpus = riscv_cpus([(0, riscv_hart(0))])
    if cpus:
        root['cpus'] = cpus
    root.update(extra)
    return fdt.blob(root)


def boot_dtb(t, dtb, variant='full', globals_='timeout: 0\n', smp=1, machine=None, cpu=None):
    m, _ = boot_variant(t, variant, extra_entry='dtb_path: boot():/boot/test.dtb', globals_=globals_, smp=smp,
                        machine=machine, cpu=cpu, files={'/boot/test.dtb': dtb})
    r = t.kernel_report(m)
    check_report_clean(t, r)
    m.stop()
    return r


@check(ports=ALL_PORTS, tags=['limine-protocol', 'dtb'])
def dtb_path_response(t):
    """dtb_path: the executable gets that tree, firmware or no firmware tree,
    its memory nodes taken out [spec]."""
    r = boot_dtb(t, tree(t, 'from dtb_path'))
    t.expect_eq(r.int('dtb.response'), 1, 'DTB response')
    t.expect_eq(r['dtb.marker'], 'from dtb_path', 'the tree dtb_path names')
    t.expect_eq(r.int('dtb.memory_nodes'), 0, 'memory nodes removed')


@check(ports=ALL_PORTS, tags=['limine-protocol', 'dtb'])
def global_dtb(t):
    """global_dtb stands for the firmware's tree for entries without a
    dtb_path of their own, which wins where given [spec]."""
    files = {'/boot/global.dtb': tree(t, 'global'), '/boot/test.dtb': tree(t, 'entry')}
    for extra, want in (('', 'global'), ('dtb_path: boot():/boot/test.dtb', 'entry')):
        m, _ = boot_variant(t, 'full', extra_entry=extra, files=files,
                            globals_='timeout: 0\nglobal_dtb: boot():/boot/global.dtb\n')
        r = t.kernel_report(m)
        check_report_clean(t, r)
        t.expect_eq(r['dtb.marker'], want, f'{extra or "no dtb_path"}: the tree used')
        m.stop()


@check(ports=ALL_PORTS, tags=['limine-protocol', 'dtb', 'hostile'])
def dtb_path_errors(t):
    """A dtb_path that does not open, or holds no valid tree, is refused, and
    the menu comes back [intent]."""
    good = tree(t, 'x')
    cases = [
        ('missing', None, 'dtb: Failed to open device tree blob with path `boot():/boot/missing.dtb`. '
                          'Is the path correct?'),
        ('not a tree', b'\0' * 256, "dtb: Invalid device tree blob at `boot():/boot/d1.dtb`: 'FDT_ERR_BADMAGIC'"),
        ('cut short', good[:len(good) // 2],
         "dtb: Invalid device tree blob at `boot():/boot/d2.dtb`: 'FDT_ERR_TRUNCATED'"),
        ('wrong version', good[:20] + (1).to_bytes(4, 'big') + good[24:],
         "dtb: Invalid device tree blob at `boot():/boot/d3.dtb`: 'FDT_ERR_BADVERSION'"),
    ]
    files, entries = {}, ''
    for i, (_, data, _) in enumerate(cases):
        path = 'boot():/boot/missing.dtb' if data is None else f'boot():/boot/d{i}.dtb'
        if data is not None:
            files[f'/boot/d{i}.dtb'] = data
        entries += entry(f'D{i}', path='boot():/boot/kernel.elf', extra=f'dtb_path: {path}')
    m, _ = boot_entry(t, 'timeout: no\n' + entries, files=files)
    for (what, _, want), got in zip(cases, panics_in_turn(t, m, len(cases))):
        t.check(got == want, f'{what}: {got!r}, want {want!r}')


def boot_smp(t, dtb=None, smp=2, variant='full', cpu=None, machine=('acpi=off',)):
    files = {} if dtb is None else {'/boot/test.dtb': dtb}
    extra = '' if dtb is None else 'dtb_path: boot():/boot/test.dtb'
    m, _ = boot_variant(t, variant, extra_entry=extra, smp=smp, machine=list(machine), cpu=cpu, files=files)
    r = t.kernel_report(m, timeout=120)
    check_report_clean(t, r)
    m.stop()
    return r


def mp(r):
    return r.int('mp.cpu_count'), r.int('mp.aps_started')


def arm_cpu(mpidr, cells=1, method='psci', **props):
    reg = (mpidr,) if cells == 1 else (mpidr >> 32, mpidr & 0xffffffff)
    node = {'device_type': 'cpu', 'compatible': 'arm,cortex-a72', 'reg': reg}
    if method is not None:
        node['enable-method'] = method
    node.update(props)
    return node


def arm_tree(t, cpus, psci=None, cells=1):
    nodes = {'#address-cells': cells, '#size-cells': 0}
    for i, node in enumerate(cpus):
        nodes[f'cpu@{i}'] = node
    extra = {}
    if psci is not False:
        extra['psci'] = psci or {'compatible': ['arm,psci-1.0', 'arm,psci-0.2', 'arm,psci'], 'method': 'hvc'}
    return tree(t, 'smp', cpus=nodes, **extra)


@check(ports=['uefi-aarch64'], tags=['limine-protocol', 'smp', 'dtb'], timeout=900)
def dtb_smp_aarch64(t):
    """Without ACPI the APs are started from the device tree, through PSCI
    by its node's method [intent]; a CPU node that cannot be started is left
    out, and a tree with no startable CPUs leaves the BSP alone."""
    t.expect_eq(mp(boot_smp(t)), (2, 1), "the firmware's tree: AP started")
    t.expect_eq(mp(boot_smp(t, arm_tree(t, [arm_cpu(0), arm_cpu(1)]))), (2, 1), 'PSCI 1.0 over HVC')
    t.expect_eq(mp(boot_smp(t, arm_tree(t, [arm_cpu(0, 2), arm_cpu(1, 2)], cells=2,
                                        psci={'compatible': 'arm,psci', 'method': 'hvc',
                                              'cpu_on': PSCI_CPU_ON_64}))),
                (2, 1), 'PSCI 0.1, its CPU_ON function ID from the node, two address cells')
    hostile = [arm_cpu(0), arm_cpu(1),
               arm_cpu(5),
               arm_cpu(1),
               arm_cpu(2, method=None),
               arm_cpu(3, method='parking'),
               arm_cpu(4, method='spin-table'),
               {'compatible': 'arm,cortex-a72', 'reg': 6, 'enable-method': 'psci'},
               {'device_type': 'cpu', 'reg': b'\0\0', 'enable-method': 'psci'}]
    t.expect_eq(mp(boot_smp(t, arm_tree(t, hostile), smp=2)), (2, 1),
                'only the AP that exists started: not one PSCI refuses (absent, already on), '
                'without or with an unknown enable-method, a spin table without a release address, '
                'not a CPU, or a short reg')
    for what, dtb in (
            ('PSCI 0.1 without CPU_ON', arm_tree(t, [arm_cpu(0), arm_cpu(1)],
                                                 psci={'compatible': 'arm,psci', 'method': 'hvc'})),
            ('PSCI without a method', arm_tree(t, [arm_cpu(0), arm_cpu(1)], psci={'compatible': 'arm,psci-1.0'})),
            ('PSCI by an unknown method', arm_tree(t, [arm_cpu(0), arm_cpu(1)],
                                                   psci={'compatible': 'arm,psci-1.0', 'method': 'svc'})),
            ('no PSCI node', arm_tree(t, [arm_cpu(0), arm_cpu(1)], psci=False)),
            ('three address cells', arm_tree(t, [arm_cpu(0), arm_cpu(1)], cells=3)),
            ('no /cpus', tree(t, 'smp', cpus=False))):
        t.expect_eq(mp(boot_smp(t, dtb))[1], 0, f'{what}: no AP started')


@check(ports=['uefi-riscv64'], tags=['limine-protocol', 'smp', 'dtb'], timeout=900)
def dtb_harts_riscv(t):
    """With a device tree (the firmware's without ACPI, or dtb_path), Limine
    takes the harts from it: those with the BSP's ISA are started, any other
    left out as a coprocessor, and those without a usable ISA string
    skipped; the BSP's mmu-type caps the paging mode [intent]."""
    t.expect_eq(mp(boot_smp(t)), (2, 1), "the firmware's tree: AP started")
    other = [e for e in RV_EXTENSIONS if e != 'h']
    harts = [(0, riscv_hart(0, mmu='riscv,sv48', **{'riscv,cbom-block-size': 32})),
             (1, riscv_hart(1, mmu='riscv,sv48')),
             (2, riscv_hart(2, extensions=other)),
             (5, riscv_hart(5, mmu='riscv,sv48')),
             (6, riscv_hart(6, extensions=None)),
             (7, riscv_hart(7, extensions=None, isa='xyz')),
             (8, {'device_type': 'cpu', 'reg': b'\0'}),
             (9, {'reg': 9})]
    r = boot_smp(t, tree(t, 'harts', cpus=riscv_cpus(harts, timebase=(0, 10000000))), variant='pagingtop',
                 machine=())
    t.expect_eq(mp(r), (2, 1), 'hart 1 started; hart 2 a coprocessor, hart 5 absent, the rest no harts')
    t.expect_eq(r.int('paging_mode'), 1, 'Sv48, as mmu-type says')
    harts = [(i, riscv_hart(i, mmu='riscv,sv39', extensions=None, isa=RV_ISA)) for i in (0, 1)]
    r = boot_smp(t, tree(t, 'isa', cpus=riscv_cpus(harts)), variant='pagingtop', machine=())
    t.expect_eq(mp(r), (2, 1), 'riscv,isa strings alone')
    t.expect_eq(r.int('paging_mode'), 0, 'Sv39, as mmu-type says')
    harts = [(i, riscv_hart(i, mmu='riscv,none')) for i in (0, 1)]
    probe_tree = tree(t, 'probe', cpus=riscv_cpus(harts, timebase=None))
    r = boot_smp(t, probe_tree, variant='pagingtop', machine=())
    t.expect_eq(r.int('paging_mode'), 2, 'no MMU type given: Sv57, the mode the firmware runs in')
    # Firmware that runs with translation off, here the EFI probe that starts
    # Limine, leaves Limine to try each mode, the largest first.
    from cases.chainload import probe_then_limine
    m = probe_then_limine(t, 'name:satp satpoff', 'timeout: 0\n' + entry(
        'Test', path='boot():/boot/kernel.elf', cmdline='x', extra='dtb_path: boot():/boot/test.dtb'),
        {'/boot/kernel.elf': HostFile(kernel_file(t, 'pagingtop')), '/boot/test.dtb': probe_tree}, smp=2)
    r = t.kernel_report(m, timeout=120)
    check_report_clean(t, r)
    t.expect_eq(r.int('paging_mode'), 2, 'translation off and no MMU type: Sv57, the largest the hart takes')


@check(ports=['uefi-riscv64'], tags=['limine-protocol', 'dtb', 'hostile'])
def dtb_harts_riscv_errors(t):
    """A dtb_path whose harts leave the BSP out or unusable is refused, and
    the menu comes back [intent]."""
    cases = [
        ('no /cpus', tree(t, 'x', cpus=False), 'riscv: missing `/cpus` node'),
        ('no BSP', tree(t, 'x', cpus=riscv_cpus([(3, riscv_hart(3))])), 'riscv: missing `struct riscv_hart` for BSP'),
        ('a 32-bit BSP', tree(t, 'x', cpus=riscv_cpus([(0, riscv_hart(0, base='rv32i'))])),
         'unsupported cpu: rv32i'),
    ]
    files, entries = {}, ''
    for i, (_, data, _) in enumerate(cases):
        files[f'/boot/d{i}.dtb'] = data
        entries += entry(f'D{i}', path='boot():/boot/kernel.elf', extra=f'dtb_path: boot():/boot/d{i}.dtb')
    m, _ = boot_entry(t, 'timeout: no\n' + entries, files=files)
    for (what, _, want), got in zip(cases, panics_in_turn(t, m, len(cases))):
        t.check(got == want, f'{what}: {got!r}, want {want!r}')


def la_tree(t, ids, cells=1):
    cpus = {'#address-cells': cells, '#size-cells': 0}
    for i, phys in enumerate(ids):
        reg = phys if isinstance(phys, bytes) else (phys,) if cells == 1 else (phys >> 32, phys & 0xffffffff)
        cpus[f'cpu@{i}'] = {'device_type': 'cpu', 'compatible': 'loongson,la464', 'reg': reg}
    return tree(t, 'smp', cpus=cpus)


@check(ports=['uefi-loongarch64'], tags=['limine-protocol', 'smp', 'dtb'], timeout=900)
def dtb_smp_loongarch(t):
    """Without ACPI the APs are started from the device tree's CPU nodes
    [intent]; core IDs the IPI cannot address, or that no core answers to,
    are left out. edk2's LoongArch platform publishes no tree of its own, so
    each comes from dtb_path."""
    t.expect_eq(mp(boot_smp(t, la_tree(t, [0, 1, 0x400, 5, b'\0']))), (2, 1),
                'core 1 started; 0x400 past the IPI field, 5 absent, a short reg not a CPU')
    t.expect_eq(mp(boot_smp(t, la_tree(t, [0, 1, 1 << 32], cells=2))), (2, 1),
                'two address cells; an ID past 32 bits skipped')
    for what, dtb in (('three address cells', la_tree(t, [0, 1], cells=3)),
                      ('no /cpus', tree(t, 'smp', cpus=False))):
        t.expect_eq(mp(boot_smp(t, dtb))[1], 0, f'{what}: no AP started')


@check(ports=['uefi-aarch64', 'uefi-riscv64', 'uefi-loongarch64'], tags=['linux', 'dtb'])
def dtb_path_linux(t):
    """dtb_path for Linux: the kernel gets that tree, its memory nodes taken
    out, /chosen filled in [spec]; LoongArch kernels find it only as the
    device tree configuration table, so it is installed as one [intent]."""
    from cases.linux import boot_linux, stub_report
    m = boot_linux(t, cmdline='with a tree', extra='dtb_path: boot():/boot/test.dtb',
                   files={'/boot/test.dtb': tree(t, 'linux tree')})
    r = stub_report(t, m)
    t.expect_eq(r['dtb.marker'], 'linux tree', 'the tree dtb_path names')
    t.expect_eq(int(r['dtb.memory_nodes'], 16), 0, 'memory nodes removed')
    if t.port.arch == 'loongarch64':
        t.check('b1b621d5-f19c-41a5-830b-d9152c69aae0' in r.multi.get('efi.table', []),
                'device tree configuration table installed')
    else:
        t.expect_eq(r['chosen.bootargs'], 'with a tree', '/chosen/bootargs')


RISCV_BOOT_PROTOCOL = 'ccd15fec-6f73-4eec-8395-3e69e4b940bf'


@check(ports=['uefi-riscv64'], tags=['dtb', 'hostile'])
def riscv_boot_hart_from_dtb(t):
    """Firmware without the RISC-V boot protocol (here, the EFI probe takes
    it away, and installs a device tree of its own, before it starts Limine):
    the boot hart is the device tree's /chosen/boot-hartid, the fallback
    RISC-V UEFI Protocol 1.0.0 chapter 3 mentions, in 32 or 64 bits; without
    a usable one Limine cannot go on [intent]."""
    from harness.media import HostFile, Tree
    from cases._common import kernel_file, wait_panic
    from cases.chainload import probe

    def chosen(hartid):
        node = {'bootargs': 'x'}
        if hartid is not None:
            node['boot-hartid'] = hartid
        return tree(t, 'boot hart', chosen=node)

    for what, dtb, boots in (('32-bit boot-hartid', chosen((0,)), True),
                             ('64-bit boot-hartid', chosen((0, 0)), True),
                             ('no boot-hartid', chosen(None), False),
                             ('a 16-bit boot-hartid', chosen(b'\0\0'), False)):
        tr = Tree()
        tr.add('/EFI/BOOT/' + t.port.efi_name, probe(t))
        tr.add('/efiprobe.cmd', f'uninstall:{RISCV_BOOT_PROTOCOL} dtb:hart.dtb '
                                f'chain:\\EFI\\limine\\{t.port.efi_name}\n')
        tr.add('/hart.dtb', dtb)
        tr.add('/EFI/limine/' + t.port.efi_name, HostFile(t.env.bin(t.port.efi_name)))
        tr.add('/EFI/limine/limine.conf', 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x'))
        tr.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
        m = t.boot(t.boot_disk(tr))
        if boots:
            r = t.kernel_report(m)
            check_report_clean(t, r)
            t.expect_eq(r.int('riscv_bsp_hartid'), 0, f'{what}: the BSP hart ID')
        else:
            msg, _ = wait_panic(t, m)
            t.expect_eq(msg, "failed to get BSP's hartid", what)
        m.stop()
