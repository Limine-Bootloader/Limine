from harness.api import check
from harness.ports import ALL_PORTS
from cases._common import boot_entry, entry, panics_in_turn

# Module paths that are not URIs Limine can use: each panic names the fault
# [intent], and the menu comes back.

GUID = '01234567-89ab-cdef-0123-456789abcdef'


def failed(uri):
    return f'limine: Failed to open module with path `{uri}`. Is the path correct?'


CASES = [
    ('hash of no known length', 'boot():/boot/m#abc',
     'Hash must be 64 BLAKE3, 128 BLAKE2b, or 256 BLAKE3 characters long'),
    ('hash with a non-hex digit', 'boot():/boot/m#' + '0' * 5 + 'z' + '0' * 58,
     'Hash contains invalid character at position 5'),
    ('hdd without a drive', 'hdd(:1):/boot/m', 'Drive number cannot be omitted for hdd():/ and odd():/'),
    ('odd without a drive', 'odd(:1):/boot/m', 'Drive number cannot be omitted for hdd():/ and odd():/'),
    ('drive 0', 'hdd(0:1):/boot/m', 'Drive number outside range 1-256'),
    ('drive 257', 'odd(257:0):/boot/m', 'Drive number outside range 1-256'),
    ('partition 257', 'hdd(1:257):/boot/m', 'Partition number outside range 0-256'),
    ('boot partition 257', 'boot(257):/boot/m', 'Partition number outside range 0-256'),
    ('not an IPv4 address', 'tftp(1.2.3):/boot/m', 'tftp: Invalid ipv4 address: 1.2.3'),
    ('unknown resource', 'nonsense():/boot/m', 'Resource `nonsense` not valid.'),
    ('empty resource', '():/boot/m', 'Resource `` not valid.'),
    ('compressed unknown resource', '$nonsense():/boot/m', 'Resource `nonsense` not valid.'),
    ('no such GUID', f'guid({GUID}):/boot/m', failed(f'guid({GUID}):/boot/m')),
    ('no such UUID', f'uuid({GUID}):/boot/m', failed(f'uuid({GUID}):/boot/m')),
    ('not a GUID', 'guid(nonsense):/boot/m', failed('guid(nonsense):/boot/m')),
    ('no such label', 'fslabel(NO SUCH LABEL):/boot/m', failed('fslabel(NO SUCH LABEL):/boot/m')),
    ('no such drive', 'hdd(200:1):/boot/m', failed('hdd(200:1):/boot/m')),
    ('no such partition', 'boot(9):/boot/m', failed('boot(9):/boot/m')),
    ('no root', 'boot/m', failed('boot/m')),
    ('no path', 'boot():/', failed('boot():/')),
]


@check(ports=ALL_PORTS, tags=['uri', 'hostile'], timeout=600)
def uri_hostile(t):
    """Each bad module URI ends in a panic that names what is wrong."""
    entries = ''.join(entry(f'U{i:02}', path='boot():/boot/kernel.elf', modules=[(uri, None)])
                      for i, (_, uri, _) in enumerate(CASES))
    m, _ = boot_entry(t, 'timeout: no\n' + entries, files={'/boot/m': b'module\n'})
    for (what, _, want), got in zip(CASES, panics_in_turn(t, m, len(CASES))):
        t.check(got == want, f'{what}: {got!r}, want {want!r}')
