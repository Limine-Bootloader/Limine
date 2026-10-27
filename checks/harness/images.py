"""Small images in the formats Limine reads wallpapers from, for checks that
know exactly which colour should land on which pixel. pixels is row-major,
top row first, each 0xRRGGBB."""

import struct
import zlib


def _rgb(p):
    return bytes(((p >> 16) & 255, (p >> 8) & 255, p & 255))


def bmp(width, height, pixels):
    """24 bits per pixel, bottom-up, rows padded to 4 bytes."""
    stride = (width * 3 + 3) & ~3
    rows = []
    for y in reversed(range(height)):
        row = b''.join(_rgb(p)[::-1] for p in pixels[y * width:(y + 1) * width])
        rows.append(row + b'\0' * (stride - len(row)))
    body = b''.join(rows)
    info = struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0, len(body), 2835, 2835, 0, 0)
    head = struct.pack('<2sIHHI', b'BM', 14 + len(info) + len(body), 0, 0, 14 + len(info))
    return head + info + body


def png(width, height, pixels):
    raw = b''.join(b'\0' + b''.join(_rgb(p) for p in pixels[y * width:(y + 1) * width]) for y in range(height))

    def chunk(tag, body):
        return struct.pack('>I', len(body)) + tag + body + struct.pack('>I', zlib.crc32(tag + body))

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def qoi(width, height, pixels):
    """Every pixel as a QOI_OP_RGB chunk, which any decoder must take."""
    body = b''.join(b'\xfe' + _rgb(p) for p in pixels)
    return b'qoif' + struct.pack('>IIBB', width, height, 3, 0) + body + b'\0' * 7 + b'\1'


def qoi_chunks(width, height, chunks, channels=4):
    """A QOI image (QOI specification 1.0) of the given chunks, and the
    pixels they decode to, by the specification's rules. Each chunk is
    ('rgb', r, g, b), ('rgba', r, g, b, a), ('index', i), ('diff', dr, dg,
    db), ('luma', dg, dr - dg, db - dg) or ('run', n)."""
    px, index, pixels, body = (0, 0, 0, 255), [(0, 0, 0, 0)] * 64, [], bytearray()
    for kind, *v in chunks:
        n = 1
        if kind == 'rgb':
            body += bytes((0xfe, *v))
            px = (*v, px[3])
        elif kind == 'rgba':
            body += bytes((0xff, *v))
            px = tuple(v)
        elif kind == 'index':
            body.append(v[0])
            px = index[v[0]]
        elif kind == 'diff':
            body.append(0x40 | (v[0] + 2) << 4 | (v[1] + 2) << 2 | (v[2] + 2))
            px = tuple((c + d) & 255 for c, d in zip(px, v)) + (px[3],)
        elif kind == 'luma':
            dg, dr_dg, db_dg = v
            body += bytes((0x80 | (dg + 32), (dr_dg + 8) << 4 | (db_dg + 8)))
            px = ((px[0] + dg + dr_dg) & 255, (px[1] + dg) & 255, (px[2] + dg + db_dg) & 255, px[3])
        elif kind == 'run':
            body.append(0xc0 | (v[0] - 1))
            n = v[0]
        index[(px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64] = px
        pixels += [px] * n
    assert len(pixels) == width * height, f'{len(pixels)} pixels for {width}x{height}'
    data = b'qoif' + struct.pack('>IIBB', width, height, channels, 0) + bytes(body) + b'\0' * 7 + b'\1'
    return data, [r << 16 | g << 8 | b for r, g, b, _ in pixels]


def jpeg_grey(width, height, level):
    """A baseline greyscale JPEG of one level throughout. Every 8x8 block holds
    only a DC coefficient, which with a quantiser of 1 decodes exactly."""
    dc = 8 * (level - 128)
    size = abs(dc).bit_length()
    blocks = ((width + 7) // 8) * ((height + 7) // 8)
    # DC codes: 00 for a difference of 0, 01 for the first block's category.
    # The only AC code is 0, for end of block.
    bits = []
    for i in range(blocks):
        if i == 0 and size:
            bits += [0, 1]
            value = dc if dc > 0 else dc + (1 << size) - 1
            bits += [(value >> (size - 1 - b)) & 1 for b in range(size)]
        else:
            bits += [0, 0]
        bits.append(0)
    bits += [1] * (-len(bits) % 8)
    data = bytearray()
    for i in range(0, len(bits), 8):
        byte = int(''.join(map(str, bits[i:i + 8])), 2)
        data.append(byte)
        if byte == 0xff:
            data.append(0)

    def segment(marker, body):
        return struct.pack('>BBH', 0xff, marker, len(body) + 2) + body

    dc_counts = [0, 2] + [0] * 14 if size else [0, 1] + [0] * 14
    dc_values = [0, size] if size else [0]
    return (b'\xff\xd8'
            + segment(0xdb, b'\x00' + b'\x01' * 64)
            + segment(0xc0, struct.pack('>BHHB', 8, height, width, 1) + b'\x01\x11\x00')
            + segment(0xc4, b'\x00' + bytes(dc_counts) + bytes(dc_values))
            + segment(0xc4, b'\x10' + bytes([1] + [0] * 15) + b'\x00')
            + segment(0xda, b'\x01\x01\x00\x00\x3f\x00')
            + bytes(data) + b'\xff\xd9')
