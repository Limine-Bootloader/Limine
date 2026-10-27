import gzip

from harness.api import check
from harness.media import HostFile, Tree
from harness.ports import ALL_PORTS, Drive
from cases._common import check_report_clean, entry, kernel_file, wait_panic

# Disks that fail Limine's reads: sectors the medium will not deliver, and a
# disk smaller than the transfers Limine sizes up its drives with.

MODULE = bytes(range(251)) * (2 * 1024 * 1024 // 251)


def failing_disk(t, files, config):
    """A boot disk holding files, through QEMU's blkdebug driver failing every
    read that covers the fifth sector of the last one's data; a megabyte of
    padding keeps the failing sector clear of the firmware's and Limine's
    reads ahead of what comes before."""
    tree = Tree()
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
    t.limine_files(config, tree)
    tree.add('/boot/padding', bytes(1 << 20))
    for path, data in files.items():
        tree.add(path, data)
    disk = t.boot_disk(tree)
    with open(disk.path, 'rb') as f:
        image = f.read()
    last = list(files.values())[-1]
    at = image.find(last[:4096])
    t.require(at > 0, 'the last file found in the image')
    rules = t.path('blkdebug.conf')
    with open(rules, 'w') as f:
        f.write(f'[inject-error]\nevent = "read_aio"\nerrno = "5"\nsector = "{at // 512 + 4}"\n')
    return f'blkdebug:{rules}:{disk.path}'


@check(ports=ALL_PORTS, tags=['disk', 'hostile'])
def disk_read_errors(t):
    """A module whose data the medium cannot deliver ends the boot with a
    panic [intent], whether read as it is or inflated."""
    for path, data in (('boot():/boot/m.bin', MODULE), ('$boot():/boot/m.bin', gzip.compress(MODULE, 0, mtime=0))):
        config = 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', modules=[(path, None)])
        disk = failing_disk(t, {'/boot/m.bin': data}, config)
        m = t.boot(drives=[Drive(disk, bootindex=1)])
        msg, _ = wait_panic(t, m)
        t.expect_eq(msg, 'fat32: cluster chain read failed (corrupted filesystem?)', f'{path}: the failed read')
        m.stop()


@check(ports=['bios'], tags=['disk'])
def disk_smaller_than_transfers(t):
    """A disk smaller than the 32KiB Limine reads to size up its transfers
    does not stop the boot [intent]."""
    tiny = t.path('tiny.img')
    with open(tiny, 'wb') as f:
        f.write(b'\0' * 510 + b'\x55\xaa' + b'\0' * (16384 - 512))
    tree = Tree()
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
    t.limine_files('timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x'), tree)
    disk = t.boot_disk(tree)
    m = t.boot(drives=[Drive(disk.path, bootindex=1), Drive(tiny)])
    r = t.kernel_report(m)
    check_report_clean(t, r)
