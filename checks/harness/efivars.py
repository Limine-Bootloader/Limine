"""UEFI variable stores for the checks: a port's template with chosen variables
set beforehand, and what a store holds after a machine ran on it, both by way
of virt-fw-vars."""

import json
import os
import struct
import subprocess

from harness.env import which

GLOBAL = '8be4df61-93ca-11d2-aa0d-00e098032b8c'
# The Boot Loader Interface's vendor.
LOADER = '4a67b082-0a4c-41cf-b6c7-440b29bb8c4f'
NV_BS_RT = 7
LOAD_OPTION_ACTIVE = 1


def utf16z(text):
    return (text + '\0').encode('utf-16-le')


def load_option(description, path, data=b''):
    """An EFI_LOAD_OPTION for a file named by a short-form device path, its
    file path alone, which firmware looks for on every filesystem."""
    name = utf16z(path)
    dp = struct.pack('<BBH', 4, 4, 4 + len(name)) + name + struct.pack('<BBH', 0x7f, 0xff, 4)
    return struct.pack('<IH', LOAD_OPTION_ACTIVE, len(dp)) + utf16z(description) + dp + data


def _run(argv):
    res = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if res.returncode != 0:
        raise RuntimeError(f'command failed ({res.returncode}): {" ".join(argv)}\n{res.stdout.decode()}')


def store(template, path, variables):
    """A copy of template with variables set: (name, guid, data) each, with
    the attributes an OS sets persistent variables with."""
    doc = {'version': 2, 'variables': [{'name': name, 'guid': guid, 'attr': NV_BS_RT, 'data': data.hex()}
                                       for name, guid, data in variables]}
    with open(path + '.json', 'w') as f:
        json.dump(doc, f)
    _run([which('virt-fw-vars'), '-i', template, '-o', path, '--set-json', path + '.json'])
    return path


def read(path):
    """{(name, guid): data} of the variables in a store."""
    out = path + '.read.json'
    _run([which('virt-fw-vars'), '-i', path, '--output-json', out])
    with open(out) as f:
        doc = json.load(f)
    os.unlink(out)
    return {(v['name'], v['guid']): bytes.fromhex(v['data']) for v in doc['variables']}
