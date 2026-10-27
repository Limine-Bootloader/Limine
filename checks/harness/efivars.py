"""UEFI variable stores for the checks: a port's template with chosen variables
set beforehand, and what a store holds after a machine ran on it.

A store is a firmware volume (PI 1.10 Volume III section 3.2.1.1) whose body
holds the variables as the firmware lays them out: a 28-byte header, its size
counted from its start, then a record for each variable at a 4-byte boundary.
Gaps and free space are erased flash, all ones."""

import struct
import uuid

GLOBAL = '8be4df61-93ca-11d2-aa0d-00e098032b8c'
# The Boot Loader Interface's vendor.
LOADER = '4a67b082-0a4c-41cf-b6c7-440b29bb8c4f'
NV_BS_RT = 7
LOAD_OPTION_ACTIVE = 1

STORE_HEADER = 28
# A start marker, the record's state, a reserved byte, the attributes, a
# monotonic count, a timestamp, a public key index, the sizes of the name and
# the data, and the vendor GUID. The name, UTF-16 with its terminator, comes
# next, and the data right after it.
RECORD = struct.Struct('<HBBIQ16sIII16s')
START = 0x55aa
# A record being replaced reads IN_DELETION until its successor is in place.
ADDED = 0x3f
IN_DELETION = 0x3e
NO_TIME = bytes(16)


def utf16z(text):
    return (text + '\0').encode('utf-16-le')


def load_option(description, path, data=b''):
    """An EFI_LOAD_OPTION for a file named by a short-form device path, its
    file path alone, which firmware looks for on every filesystem."""
    name = utf16z(path)
    dp = struct.pack('<BBH', 4, 4, 4 + len(name)) + name + struct.pack('<BBH', 0x7f, 0xff, 4)
    return struct.pack('<IH', LOAD_OPTION_ACTIVE, len(dp)) + utf16z(description) + dp + data


def efi_time(year, month, day, hour=0, minute=0, second=0):
    """An EFI_TIME (UEFI 2.11 section 8.3.1), in no particular time zone."""
    return struct.pack('<HBBBBBBIhBB', year, month, day, hour, minute, second, 0, 0, 0, 0, 0)


def _area(image):
    """(start, end) of a store image's records."""
    if image[40:44] != b'_FVH':
        raise ValueError('not a firmware volume')
    header_length = struct.unpack_from('<H', image, 48)[0]
    size = struct.unpack_from('<I', image, header_length + 16)[0]
    return header_length + STORE_HEADER, header_length + size


def _records(image):
    p, end = _area(image)
    while p + RECORD.size <= end:
        start, state, _, attributes, _, timestamp, _, name_size, data_size, guid = RECORD.unpack_from(image, p)
        if start != START:
            return
        name = bytes(image[p + RECORD.size:p + RECORD.size + name_size]).decode('utf-16-le').rstrip('\0')
        data = p + RECORD.size + name_size
        value = (attributes, timestamp, bytes(image[data:data + data_size]))
        yield state, name, str(uuid.UUID(bytes_le=bytes(guid))), value
        p = (data + data_size + 3) & ~3


def contents(image):
    """{(name, guid): (attributes, timestamp, data)} of the variables a store
    image holds."""
    current, replaced = {}, {}
    for state, name, guid, value in _records(image):
        if state == ADDED:
            current[(name, guid)] = value
        elif state == IN_DELETION:
            replaced[(name, guid)] = value
    # A variable caught mid-update is the record being replaced until its
    # successor is in.
    for key, value in replaced.items():
        current.setdefault(key, value)
    return current


def _record(name, guid, attributes, timestamp, data):
    name = utf16z(name)
    record = RECORD.pack(START, ADDED, 0, attributes, 0, timestamp, 0, len(name), len(data),
                         uuid.UUID(guid).bytes_le) + name + data
    return record + b'\xff' * (-len(record) % 4)


def write(template, path, values):
    """A copy of template with values set: {(name, guid): (attributes,
    timestamp, data)}. The store comes out with the template's variables and
    these in order of name."""
    with open(template, 'rb') as f:
        image = bytearray(f.read())
    start, end = _area(image)
    merged = contents(image)
    merged.update(values)
    records = b''.join(_record(name, guid, *value) for (name, guid), value in sorted(merged.items()))
    if len(records) > end - start:
        raise ValueError(f'{len(records)} bytes of variables do not fit {template}')
    image[start:end] = records + b'\xff' * (end - start - len(records))
    with open(path, 'wb') as f:
        f.write(image)
    return path


def store(template, path, variables):
    """A copy of template with variables set: (name, guid, data) each, with
    the attributes an OS sets persistent variables with."""
    return write(template, path, {(name, guid): (NV_BS_RT, NO_TIME, data) for name, guid, data in variables})


def read(path):
    """{(name, guid): data} of the variables in a store."""
    with open(path, 'rb') as f:
        return {key: data for key, (_, _, data) in contents(f.read()).items()}
