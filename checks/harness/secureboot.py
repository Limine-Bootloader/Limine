"""UEFI Secure Boot for the tests: one set of test keys (PK, KEK, db), Limine
and payloads signed with the db key, and variable stores with the keys
enrolled and Secure Boot on, from the test firmware's -secboot images."""

import fcntl
import os
import subprocess

from harness import authenticode
from harness.env import which

OWNER = '6106575a-aa8e-41ca-b306-52596bcf4244'


def _run(argv):
    res = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if res.returncode != 0:
        raise RuntimeError(f'command failed ({res.returncode}): {" ".join(argv)}\n{res.stdout.decode()}')


def keys(env):
    """The test keys, made once per build directory: {name: (key, cert)}."""
    d = os.path.join(env.payloaddir, 'secureboot')
    out = {n: (os.path.join(d, f'{n}.key'), os.path.join(d, f'{n}.pem')) for n in ('PK', 'KEK', 'db')}
    os.makedirs(d, exist_ok=True)
    # Tests run in processes of their own, and must all sign with one key.
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


def enrolled_vars(env, port, path):
    """A variable store with the test keys enrolled and Secure Boot enabled."""
    _, template = firmware(env, port)
    k = keys(env)
    _run([which('virt-fw-vars'), '-i', template, '-o', path,
          '--set-pk', OWNER, k['PK'][1], '--add-kek', OWNER, k['KEK'][1], '--add-db', OWNER, k['db'][1], '--sb'])
    return path


def sign(env, src, dest):
    """src signed with the db key."""
    key, cert = keys(env)['db']
    return authenticode.sign(src, dest, key, cert)
