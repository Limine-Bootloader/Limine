"""Authenticode signing of PE images: the image hash, a PKCS #7 SignedData
around it, and the attribute certificate table it goes in, as Microsoft's
"Windows Authenticode Portable Executable Signature Format", Version 1.0,
lays them out. openssl only makes the RSA signature, so any machine type can
be signed, which sbsign refuses for some (LoongArch among them)."""

import hashlib
import struct
import subprocess

from harness.env import which

OID_SHA256 = '2.16.840.1.101.3.4.2.1'
OID_RSA = '1.2.840.113549.1.1.1'
OID_SIGNED_DATA = '1.2.840.113549.1.7.2'
OID_CONTENT_TYPE = '1.2.840.113549.1.9.3'
OID_MESSAGE_DIGEST = '1.2.840.113549.1.9.4'
OID_SPC_INDIRECT_DATA = '1.3.6.1.4.1.311.2.1.4'
OID_SPC_PE_IMAGE_DATA = '1.3.6.1.4.1.311.2.1.15'

WIN_CERT_REVISION_2_0 = 0x0200
WIN_CERT_TYPE_PKCS_SIGNED_DATA = 0x0002


def _length(n):
    if n < 0x80:
        return bytes([n])
    body = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    return bytes([0x80 | len(body)]) + body


def tlv(tag, value):
    return bytes([tag]) + _length(len(value)) + value


def seq(*items):
    return tlv(0x30, b''.join(items))


def oid(dotted):
    parts = [int(p) for p in dotted.split('.')]
    body = bytes([40 * parts[0] + parts[1]])
    for p in parts[2:]:
        chunk = [p & 0x7f]
        p >>= 7
        while p:
            chunk.append(0x80 | (p & 0x7f))
            p >>= 7
        body += bytes(reversed(chunk))
    return tlv(0x06, body)


def algorithm(dotted):
    return seq(oid(dotted), b'\x05\x00')


def parse(data, offset=0):
    """(tag, offset of the value, offset past it) of the TLV at offset."""
    tag = data[offset]
    n = data[offset + 1]
    start = offset + 2
    if n & 0x80:
        count = n & 0x7f
        n = int.from_bytes(data[start:start + count], 'big')
        start += count
    return tag, start, start + n


def children(data, start, end):
    out = []
    while start < end:
        tag, vstart, vend = parse(data, start)
        out.append((tag, start, vstart, vend))
        start = vend
    return out


def issuer_and_serial(cert):
    """The issuer Name and serialNumber of a DER certificate, as encoded."""
    _, cstart, cend = parse(cert)
    _, tstart, tend = parse(cert, cstart)
    fields = children(cert, tstart, tend)
    if fields[0][0] == 0xa0:
        fields = fields[1:]
    serial = cert[fields[0][1]:fields[0][3]]
    issuer = cert[fields[2][1]:fields[2][3]]
    return issuer, serial


def image_digest(image):
    """The Authenticode SHA-256 of an image with no certificate table yet."""
    lfanew = struct.unpack_from('<I', image, 0x3c)[0]
    opt = lfanew + 24
    sections, opt_size = struct.unpack_from('<H', image, lfanew + 6)[0], struct.unpack_from('<H', image, lfanew + 20)[0]
    magic = struct.unpack_from('<H', image, opt)[0]
    checksum = opt + 64
    security = opt + (144 if magic == 0x20b else 128)
    headers_size = struct.unpack_from('<I', image, opt + 60)[0]
    h = hashlib.sha256()
    h.update(image[:checksum])
    h.update(image[checksum + 4:security])
    h.update(image[security + 8:headers_size])
    table = opt + opt_size
    raw = []
    for i in range(sections):
        size, pointer = struct.unpack_from('<II', image, table + 40 * i + 16)
        if size:
            raw.append((pointer, size))
    # What follows the sections starts, as the format has it and verifiers
    # take it, at the count of bytes hashed so far.
    hashed = headers_size
    for pointer, size in sorted(raw):
        h.update(image[pointer:pointer + size])
        hashed += size
    h.update(image[hashed:])
    return h.digest(), security


def sign(src, dest, key, cert_pem):
    with open(src, 'rb') as f:
        image = f.read()
    # The table goes on an 8 byte boundary, and the padding to it is part of
    # the image the hash covers.
    image += b'\0' * (-len(image) % 8)
    digest, security = image_digest(image)
    if struct.unpack_from('<II', image, security) != (0, 0):
        raise ValueError(f'{src} is signed already')

    cert = subprocess.run([which('openssl'), 'x509', '-in', cert_pem, '-outform', 'DER'],
                          stdout=subprocess.PIPE, check=True).stdout
    indirect = seq(
        seq(oid(OID_SPC_PE_IMAGE_DATA),
            # SpcPeImageData: no flags, and the file link everyone sends.
            seq(tlv(0x03, b'\x00'), tlv(0xa0, tlv(0xa2, tlv(0x80, '<<<Obsolete>>>'.encode('utf-16-be')))))),
        seq(algorithm(OID_SHA256), tlv(0x04, digest)))
    # The message digest is that of the content's value, without its tag
    # and length.
    _, vstart, _ = parse(indirect)
    attributes = [
        seq(oid(OID_CONTENT_TYPE), tlv(0x31, oid(OID_SPC_INDIRECT_DATA))),
        seq(oid(OID_MESSAGE_DIGEST), tlv(0x31, tlv(0x04, hashlib.sha256(indirect[vstart:]).digest()))),
    ]
    signed_attributes = b''.join(sorted(attributes))
    signature = subprocess.run([which('openssl'), 'dgst', '-sha256', '-sign', key, '-binary'],
                               input=tlv(0x31, signed_attributes), stdout=subprocess.PIPE, check=True).stdout
    issuer, serial = issuer_and_serial(cert)
    signer = seq(tlv(0x02, b'\x01'), seq(issuer, serial), algorithm(OID_SHA256),
                 tlv(0xa0, signed_attributes), algorithm(OID_RSA), tlv(0x04, signature))
    signed_data = seq(tlv(0x02, b'\x01'), tlv(0x31, algorithm(OID_SHA256)),
                      seq(oid(OID_SPC_INDIRECT_DATA), tlv(0xa0, indirect)),
                      tlv(0xa0, cert), tlv(0x31, signer))
    pkcs7 = seq(oid(OID_SIGNED_DATA), tlv(0xa0, signed_data))
    pkcs7 += b'\0' * (-len(pkcs7) % 8)
    entry = struct.pack('<IHH', 8 + len(pkcs7), WIN_CERT_REVISION_2_0, WIN_CERT_TYPE_PKCS_SIGNED_DATA) + pkcs7
    out = bytearray(image)
    struct.pack_into('<II', out, security, len(image), len(entry))
    out += entry
    with open(dest, 'wb') as f:
        f.write(out)
    return dest
