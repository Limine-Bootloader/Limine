"""MBR and GPT partition tables for disk images, laid out the way sfdisk lays
them out: partitions on 1MiB boundaries, and each logical partition 1MiB after
the EBR that describes it. Specs are dicts with 'start' and 'sectors' (either
may be None), 'type', 'guid', 'name', 'bootable', and 'logical'."""

import random
import struct
import uuid
import zlib

GRAIN = 1 << 20
GPT_ENTRIES = 128
GPT_ENTRY_SIZE = 128


def _align_up(value, align):
    return -(-value // align) * align


def _chs(lba):
    # The geometry sfdisk assumes, with every address past it pinned to the
    # last one CHS can give.
    heads, sectors = 255, 63
    cyl = lba // (heads * sectors)
    if cyl > 1023:
        return b'\xfe\xff\xff'
    head = lba // sectors % heads
    sector = lba % sectors + 1
    return bytes((head, sector | (cyl >> 2 & 0xc0), cyl & 0xff))


def _mbr_entry(boot, ptype, lba, sectors, chs_base=0):
    if sectors == 0:
        return bytes(16)
    absolute = chs_base + lba
    return (bytes((0x80 if boot else 0,)) + _chs(absolute) + bytes((ptype,)) + _chs(absolute + sectors - 1)
            + struct.pack('<II', lba, sectors))


def _place(specs, lowest, last, grain):
    """Fills in the start and size sfdisk would give each partition: after the
    one before it, a logical one aside, on the next grain, and up to the last
    whole grain the disk holds."""
    cursor = grain
    for s in specs:
        start = s['start']
        if start is None:
            if s.get('logical'):
                raise ValueError('a logical partition needs a start')
            start = _align_up(cursor, grain)
        sectors = s['sectors']
        if sectors is None:
            sectors = (last + 1) // grain * grain - start
        if start < lowest or sectors <= 0 or start + sectors - 1 > last:
            raise ValueError(f'partition at {start} of {sectors} sectors does not fit {lowest}-{last}')
        s['start'], s['sectors'] = start, sectors
        if not s.get('logical'):
            cursor = start + sectors


def _is_extended(s):
    return s['type'] in (0x05, 0x0f, 0x85)


def write_mbr(f, total, ss, specs, disk_id=None):
    """Writes an MBR, and EBRs for the logical partitions, to the open image
    f of total sectors. Returns the disk ID and each partition's number."""
    grain = GRAIN // ss
    if disk_id is None:
        disk_id = random.getrandbits(32)
    for s in specs:
        s['type'] = int(str(s['type']), 16)
    _place(specs, 1, total - 1, grain)
    primaries = [s for s in specs if not s['logical']]
    logicals = [s for s in specs if s['logical']]
    if len(primaries) > 4:
        raise ValueError('more than four primary partitions')
    ends = sorted((s['start'], s['start'] + s['sectors']) for s in primaries)
    if any(b[0] < a[1] for a, b in zip(ends, ends[1:])):
        raise ValueError('primary partitions overlap')
    numbers = {}
    for i, s in enumerate(primaries):
        numbers[id(s)] = i + 1
    for i, s in enumerate(logicals):
        numbers[id(s)] = i + 5
    mbr = bytearray(ss)
    struct.pack_into('<I', mbr, 0x1b8, disk_id)
    for i, s in enumerate(primaries):
        mbr[0x1be + 16 * i:0x1be + 16 * (i + 1)] = _mbr_entry(s['bootable'], s['type'], s['start'], s['sectors'])
    mbr[510:512] = b'\x55\xaa'
    f.seek(0)
    f.write(mbr)
    if logicals:
        ext = next(s for s in primaries if _is_extended(s))
        if any(not ext['start'] < s['start'] < ext['start'] + ext['sectors'] for s in logicals):
            raise ValueError('a logical partition starts outside the extended one')
        for i, s in enumerate(logicals):
            ebr_lba = s['start'] - grain
            ebr = bytearray(ss)
            ebr[0x1be:0x1ce] = _mbr_entry(s['bootable'], s['type'], s['start'] - ebr_lba, s['sectors'], ebr_lba)
            if i + 1 < len(logicals):
                nxt = logicals[i + 1]
                nxt_ebr = nxt['start'] - grain
                ebr[0x1ce:0x1de] = _mbr_entry(False, 0x05, nxt_ebr - ext['start'],
                                              nxt['start'] + nxt['sectors'] - nxt_ebr, ext['start'])
            ebr[510:512] = b'\x55\xaa'
            f.seek(ebr_lba * ss)
            f.write(ebr)
    return disk_id, [numbers[id(s)] for s in specs]


def _gpt_header(total, ss, disk_guid, entries_crc, backup):
    array_sectors = GPT_ENTRIES * GPT_ENTRY_SIZE // ss
    my, alt = (total - 1, 1) if backup else (1, total - 1)
    entries_lba = total - 1 - array_sectors if backup else 2
    fields = [b'EFI PART', 0x10000, 92, 0, 0, my, alt, GRAIN // ss, total - 2 - array_sectors,
              uuid.UUID(disk_guid).bytes_le, entries_lba, GPT_ENTRIES, GPT_ENTRY_SIZE, entries_crc]
    header = struct.pack('<8sIIIIQQQQ16sQIII', *fields)
    fields[3] = zlib.crc32(header)
    return struct.pack('<8sIIIIQQQQ16sQIII', *fields).ljust(ss, b'\0')


def write_gpt(f, total, ss, specs, disk_guid=None):
    """Writes a protective MBR, a GPT and its backup to the open image f of
    total sectors. Returns the disk GUID and each partition's GUID."""
    grain = GRAIN // ss
    array_sectors = GPT_ENTRIES * GPT_ENTRY_SIZE // ss
    disk_guid = disk_guid or str(uuid.uuid4()).upper()
    _place(specs, grain, total - 2 - array_sectors, grain)
    array = bytearray(GPT_ENTRIES * GPT_ENTRY_SIZE)
    guids = []
    for i, s in enumerate(specs):
        guid = s['guid'] or str(uuid.uuid4()).upper()
        guids.append(guid)
        name = (s['name'] or '').encode('utf-16-le')[:72].ljust(72, b'\0')
        array[i * GPT_ENTRY_SIZE:(i + 1) * GPT_ENTRY_SIZE] = struct.pack(
            '<16s16sQQQ72s', uuid.UUID(s['type']).bytes_le, uuid.UUID(guid).bytes_le, s['start'],
            s['start'] + s['sectors'] - 1, 0, name)
    entries_crc = zlib.crc32(array)
    pmbr = bytearray(ss)
    pmbr[0x1be:0x1ce] = (b'\x00\x00\x02\x00\xee\xff\xff\xff' + struct.pack('<II', 1, min(total - 1, 0xffffffff)))
    pmbr[510:512] = b'\x55\xaa'
    f.seek(0)
    f.write(pmbr)
    f.write(_gpt_header(total, ss, disk_guid, entries_crc, False))
    f.write(array)
    f.seek((total - 1 - array_sectors) * ss)
    f.write(array)
    f.write(_gpt_header(total, ss, disk_guid, entries_crc, True))
    return disk_guid, guids
