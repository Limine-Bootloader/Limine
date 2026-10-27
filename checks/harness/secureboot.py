"""UEFI Secure Boot for the checks: one set of keys (PK, KEK, db), Limine and
payloads signed with the db key, and variable stores with the keys enrolled
and Secure Boot on, from the firmware's -secboot images."""

import base64
import fcntl
import hashlib
import os
import re
import struct
import subprocess
import uuid

from harness import authenticode, efivars
from harness.env import which

OWNER = '6106575a-aa8e-41ca-b306-52596bcf4244'
# UEFI 2.11 sections 32.7.1 and 32.4.1.
IMAGE_SECURITY = 'd719b2cb-3d3a-4596-a3bc-dad00e67656f'
CERT_X509 = 'a5c059a1-94e4-4aa7-87b5-ab155c2bf072'
CERT_SHA256 = 'c1c41626-504c-4092-aca9-41f936934328'
# Time-based authenticated writes on top of NV_BS_RT (UEFI 2.11 section 8.2.1).
AUTHENTICATED = efivars.NV_BS_RT | 0x20
NV_BS = 3
# The firmware's own switches: Secure Boot enabled, its custom mode off.
SECURE_BOOT_ENABLE = ('SecureBootEnable', 'f0a30bc7-af08-4556-99c4-001009c93a44')
CUSTOM_MODE = ('CustomMode', 'c076ec0c-7028-4399-a072-71ee5c448b9f')
# dbx holds one hash, of nothing, which no image has.
DBX_OWNER = 'a0baa8a3-041d-48a8-bc87-c36d121b5e3d'
DBX_TIME = efivars.efi_time(2010, 1, 1)


def _run(argv):
    res = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if res.returncode != 0:
        raise RuntimeError(f'command failed ({res.returncode}): {" ".join(argv)}\n{res.stdout.decode()}')


def keys(env):
    """The checks' keys, made once per build directory: {name: (key, cert)}."""
    d = os.path.join(env.payloaddir, 'secureboot')
    out = {n: (os.path.join(d, f'{n}.key'), os.path.join(d, f'{n}.pem')) for n in ('PK', 'KEK', 'db')}
    os.makedirs(d, exist_ok=True)
    # Checks run in processes of their own, and must all sign with one key.
    with open(os.path.join(d, '.lock'), 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for name, (key, cert) in out.items():
            if not os.path.exists(cert):
                _run([which('openssl'), 'req', '-new', '-x509', '-newkey', 'rsa:2048', '-nodes', '-sha256',
                      '-days', '3650', '-subj', f'/CN=Limine test {name}/', '-keyout', key, '-out', cert])
    return out


def firmware(env, port):
    """(code, vars template) of the port's Secure Boot firmware, or None."""
    code = env.ovmf(port.code.replace('.fd', '-secboot.fd'))
    vars_ = env.ovmf(port.vars.replace('.fd', '-secboot.fd'))
    if os.path.exists(code) and os.path.exists(vars_):
        return code, vars_
    return None


def machine_options(port):
    """What a machine needs to run the port's Secure Boot firmware. On x86 edk2
    refuses Secure Boot with its variables in flash anything but SMM may write,
    so the firmware is built to require SMM, and the flash is made secure."""
    if not port.is_x86:
        return {}
    return {'machine_opts': ['smm=on'], 'extra': ['-global', 'driver=cfi.pflash01,property=secure,value=on']}


def signature_list(signature_type, owner, data):
    """An EFI_SIGNATURE_LIST (UEFI 2.11 section 32.4.1) of one signature."""
    entry = uuid.UUID(owner).bytes_le + data
    return uuid.UUID(signature_type).bytes_le + struct.pack('<III', 28 + len(entry), 0, len(entry)) + entry


def _der(data, at):
    """(tag, where the contents start, where they end) of the DER element at at."""
    tag, length = data[at], data[at + 1]
    at += 2
    if length & 0x80:
        count = length & 0x7f
        length = int.from_bytes(data[at:at + count], 'big')
        at += count
    return tag, at, at + length


def not_before(der):
    """When a certificate's validity begins (RFC 5280 section 4.1.2.5), as an
    EFI_TIME."""
    _, at, _ = _der(der, 0)
    _, at, _ = _der(der, at)
    # The version, a tagged element, is optional; the serial number, the
    # signature algorithm and the issuer come before the validity.
    tag, _, end = _der(der, at)
    if tag == 0xa0:
        at = end
    for _ in range(3):
        at = _der(der, at)[2]
    _, at, _ = _der(der, at)
    tag, start, end = _der(der, at)
    text = der[start:end].decode()
    if tag == 0x17:
        year = 2000 + int(text[:2]) if int(text[:2]) < 50 else 1900 + int(text[:2])
        text = text[2:]
    else:
        year = int(text[:4])
        text = text[4:]
    return efivars.efi_time(year, *(int(text[i:i + 2]) for i in range(0, 10, 2)))


def _certificate(pem):
    """(timestamp, signature list) of a PEM certificate, stamped with when it
    becomes valid."""
    with open(pem) as f:
        body = re.search(r'-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----', f.read(), re.S).group(1)
    der = base64.b64decode(''.join(body.split()))
    return not_before(der), signature_list(CERT_X509, OWNER, der)


def enrolled_vars(env, port, path):
    """A variable store with the keys enrolled and Secure Boot enabled."""
    _, template = firmware(env, port)
    k = keys(env)
    values = {name: (AUTHENTICATED, *_certificate(k[key][1]))
              for name, key in ((('PK', efivars.GLOBAL), 'PK'), (('KEK', efivars.GLOBAL), 'KEK'),
                                (('db', IMAGE_SECURITY), 'db'))}
    values[('dbx', IMAGE_SECURITY)] = (AUTHENTICATED, DBX_TIME,
                                       signature_list(CERT_SHA256, DBX_OWNER, hashlib.sha256().digest()))
    values[SECURE_BOOT_ENABLE] = (NV_BS, efivars.NO_TIME, b'\x01')
    values[CUSTOM_MODE] = (NV_BS, efivars.NO_TIME, b'\x00')
    return efivars.write(template, path, values)


def sign(env, src, dest):
    """src signed with the db key."""
    key, cert = keys(env)['db']
    return authenticode.sign(src, dest, key, cert)
