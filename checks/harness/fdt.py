"""Flattened device trees (Devicetree Specification v0.4, chapter 5), built
from nested dicts: a dict value is a child node, any other value a property.
A property is given as str (NUL-terminated), a list of str (a string list),
int (one 32-bit cell), a tuple of ints (32-bit cells), or bytes as they are."""

import struct

MAGIC = 0xd00dfeed
BEGIN_NODE, END_NODE, PROP, END = 1, 2, 3, 9


def _value(v):
    if isinstance(v, bytes):
        return v
    if isinstance(v, str):
        return v.encode() + b'\0'
    if isinstance(v, list):
        return b''.join(s.encode() + b'\0' for s in v)
    if isinstance(v, int):
        return struct.pack('>I', v)
    return b''.join(struct.pack('>I', c) for c in v)


def _pad(b):
    return b + bytes(-len(b) % 4)


def blob(root, boot_cpuid=0):
    strings = bytearray()
    offsets = {}

    def name_offset(name):
        if name not in offsets:
            offsets[name] = len(strings)
            strings.extend(name.encode() + b'\0')
        return offsets[name]

    def node(name, body):
        out = struct.pack('>I', BEGIN_NODE) + _pad(name.encode() + b'\0')
        for key, v in body.items():
            if not isinstance(v, dict):
                data = _value(v)
                out += struct.pack('>III', PROP, len(data), name_offset(key)) + _pad(data)
        for key, v in body.items():
            if isinstance(v, dict):
                out += node(key, v)
        return out + struct.pack('>I', END_NODE)

    structure = node('', root) + struct.pack('>I', END)
    rsvmap = bytes(16)
    off_rsvmap = 40
    off_struct = off_rsvmap + len(rsvmap)
    off_strings = off_struct + len(structure)
    total = off_strings + len(strings)
    header = struct.pack('>10I', MAGIC, total, off_struct, off_strings, off_rsvmap, 17, 16, boot_cpuid,
                         len(strings), len(structure))
    return header + rsvmap + structure + bytes(strings)
