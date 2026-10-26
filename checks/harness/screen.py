import lzma
import os
import re
import struct
import zlib

from harness.env import SRCDIR

# CP437 to Unicode, for turning decoded glyph codes into comparable text.
CP437 = (
    '\x00☺☻♥♦♣♠•◘○◙♂♀♪♫☼'
    '►◄↕‼¶§▬↨↑↓→←∟↔▲▼'
    ' !"#$%&\'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~⌂'
    'ÇüéâäàåçêëèïîìÄÅ'
    'ÉæÆôöòûùÿÖÜ¢£¥₧ƒ'
    'áíóúñÑªº¿⌐¬½¼¡«»'
    '░▒▓│┤╡╢╖╕╣║╗╝╜╛┐'
    '└┴┬├─┼╞╟╚╔╩╦╠═╬╧'
    '╨╤╥╙╘╒╓╫╪┘┌█▄▌▐▀'
    'αßΓπΣσµτΦΘΩδ∞φε∩'
    '≡±≥≤⌠⌡÷≈°∙·√ⁿ²■ '
)
assert len(CP437) == 256


class Image:
    def __init__(self, width, height, data):
        self.width = width
        self.height = height
        self.data = data

    @classmethod
    def load_ppm(cls, path):
        with open(path, 'rb') as f:
            raw = f.read()
        m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', raw)
        if not m or int(m.group(3)) != 255:
            raise ValueError(f'unsupported PPM: {path}')
        w, h = int(m.group(1)), int(m.group(2))
        data = raw[m.end():m.end() + w * h * 3]
        return cls(w, h, bytes(data))

    def pixel(self, x, y):
        i = (y * self.width + x) * 3
        d = self.data
        return (d[i] << 16) | (d[i + 1] << 8) | d[i + 2]

    def row_bytes(self, y, x0, x1):
        base = y * self.width * 3
        return self.data[base + x0 * 3:base + x1 * 3]

    def crop(self, x, y, w, h):
        rows = [self.row_bytes(yy, x, x + w) for yy in range(y, y + h)]
        return Image(w, h, b''.join(rows))

    def rotate(self, degrees):
        """Rotate clockwise by a multiple of 90 degrees."""
        degrees %= 360
        if degrees == 0:
            return self
        w, h = self.width, self.height
        px = [self.data[i:i + 3] for i in range(0, w * h * 3, 3)]
        if degrees == 180:
            return Image(w, h, b''.join(reversed(px)))
        out = []
        if degrees == 90:
            for y in range(w):
                for x in range(h):
                    out.append(px[(h - 1 - x) * w + y])
        else:
            for y in range(w):
                for x in range(h):
                    out.append(px[x * w + (w - 1 - y)])
        return Image(h, w, b''.join(out))

    def count_colour(self, rgb, region=None):
        x0, y0, x1, y1 = region or (0, 0, self.width, self.height)
        needle = bytes(((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255))
        n = 0
        for y in range(y0, y1):
            row = self.row_bytes(y, x0, x1)
            for i in range(0, len(row), 3):
                if row[i:i + 3] == needle:
                    n += 1
        return n

    def colours(self, region=None):
        x0, y0, x1, y1 = region or (0, 0, self.width, self.height)
        seen = {}
        for y in range(y0, y1):
            row = self.row_bytes(y, x0, x1)
            for i in range(0, len(row), 3):
                c = row[i:i + 3]
                seen[c] = seen.get(c, 0) + 1
        return {(c[0] << 16) | (c[1] << 8) | c[2]: n for c, n in seen.items()}

    def is_uniform(self, rgb, region=None):
        cols = self.colours(region)
        return list(cols) == [rgb]

    def save_ppm(self, path):
        with open(path, 'wb') as f:
            f.write(b'P6\n%d %d\n255\n' % (self.width, self.height))
            f.write(self.data)

    def save_png(self, path):
        stride = self.width * 3
        raw = b''.join(b'\x00' + self.data[y * stride:(y + 1) * stride] for y in range(self.height))

        def chunk(tag, body):
            return (struct.pack('>I', len(body)) + tag + body
                    + struct.pack('>I', zlib.crc32(tag + body) & 0xffffffff))

        with open(path, 'wb') as f:
            f.write(b'\x89PNG\r\n\x1a\n')
            f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', self.width, self.height, 8, 2, 0, 0, 0)))
            f.write(chunk(b'IDAT', zlib.compress(raw, 6)))
            f.write(chunk(b'IEND', b''))


_builtin_font = None


def builtin_font():
    """The font Limine draws its interface with, read from its source."""
    global _builtin_font
    if _builtin_font is None:
        with open(os.path.join(SRCDIR, 'common', 'lib', 'gterm.c')) as f:
            src = f.read()
        body = src[src.index('builtin_font[] = {'):]
        body = body[:body.index('};')]
        values = [int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})', body)]
        if len(values) != 4096:
            raise RuntimeError(f'builtin font has {len(values)} bytes, expected 4096')
        _builtin_font = bytes(values)
    return _builtin_font


class Font:
    """A VGA style font: 256 glyphs, a byte a row, as Flanterm expands it."""

    def __init__(self, data=None, height=16, spacing=1, scale_x=1, scale_y=1):
        self.data = data if data is not None else builtin_font()
        self.height = height
        self.spacing = spacing
        self.scale_x = scale_x
        self.scale_y = scale_y
        self.cell_w = (8 + spacing) * scale_x
        self.cell_h = height * scale_y
        self._table = None

    def glyph_rows(self, code):
        """The rows of a glyph as drawn, each an integer whose top bit is the
        leftmost dot, (8 + spacing) dots wide before scaling."""
        width = 8 + self.spacing
        rows = []
        for y in range(self.height):
            b = self.data[code * self.height + y]
            v = b << self.spacing
            if 0xc0 <= code <= 0xdf and b & 1:
                v |= (1 << self.spacing) - 1
            rows.append(v & ((1 << width) - 1))
        return tuple(rows)

    def table(self):
        if self._table is None:
            t = {}
            # Earlier codes win, except that space stands for every blank glyph.
            order = [0x20] + [c for c in range(256) if c != 0x20]
            for code in order:
                t.setdefault(self.glyph_rows(code), code)
            self._table = t
        return self._table


class Cell:
    __slots__ = ('ch', 'code', 'fg', 'bg')

    def __init__(self, ch, code, fg, bg):
        self.ch = ch
        self.code = code
        self.fg = fg
        self.bg = bg

    def __repr__(self):
        fg = 'None' if self.fg is None else f'{self.fg:06x}'
        bg = 'None' if self.bg is None else f'{self.bg:06x}'
        return f'Cell({self.ch!r}, fg={fg}, bg={bg})'


class TextScreen:
    """A decoded grid of cells, with helpers for assertions."""

    def __init__(self, cells, cols, rows):
        self.cells = cells
        self.cols = cols
        self.rows = rows

    def cell(self, x, y):
        return self.cells[y * self.cols + x]

    def row_text(self, y):
        return ''.join(self.cell(x, y).ch for x in range(self.cols))

    def lines(self):
        return [self.row_text(y) for y in range(self.rows)]

    def text(self):
        return '\n'.join(line.rstrip() for line in self.lines())

    def find(self, needle):
        """All (x, y) where needle starts on a single row."""
        hits = []
        for y in range(self.rows):
            line = self.row_text(y)
            start = 0
            while True:
                i = line.find(needle, start)
                if i < 0:
                    break
                hits.append((i, y))
                start = i + 1
        return hits

    def find_one(self, needle):
        hits = self.find(needle)
        if len(hits) != 1:
            raise AssertionError(f'expected exactly one {needle!r} on screen, found {hits}\n{self.text()}')
        return hits[0]

    def span_fg(self, x, y, length):
        return {self.cell(x + i, y).fg for i in range(length) if self.cell(x + i, y).ch != ' '}

    def span_bg(self, x, y, length):
        return {self.cell(x + i, y).bg for i in range(length)}

    def dump(self):
        out = []
        for y in range(self.rows):
            out.append(f'{y:3d}|{self.row_text(y)}|')
        return '\n'.join(out)


def flanterm_geometry(width, height, font, margin=0):
    cols = (width - 2 * margin) // font.cell_w
    rows = (height - 2 * margin) // font.cell_h
    off_x = margin + ((width - 2 * margin) % font.cell_w) // 2
    off_y = margin + ((height - 2 * margin) % font.cell_h) // 2
    return cols, rows, off_x, off_y


def decode_gterm(img, font=None, margin=0, cols=None, rows=None):
    """Read text back off a screenshot of Flanterm's framebuffer terminal.
    font may be a function choosing one for the picture."""
    if callable(font):
        font = font(img)
    font = font or Font()
    gcols, grows, off_x, off_y = flanterm_geometry(img.width, img.height, font, margin)
    cols = gcols if cols is None else cols
    rows = grows if rows is None else rows
    return decode_cells(img, font.table(), lambda code: CP437[code], cols, rows, off_x, off_y,
                        8 + font.spacing, font.height, font.scale_x, font.scale_y)


def decode_cells(img, table, char, cols, rows, off_x, off_y, width, height, scale_x=1, scale_y=1,
                 partial=None):
    """Match each cell of a grid against a font's glyphs.

    table maps a glyph's rows (integers, top bit leftmost) to a code that char()
    turns into text. Every cell is tried against each colour it contains as the
    foreground; the colour whose dots spell a known glyph wins, and of several
    that do, the one lighting the most dots (so that reverse video reads as its
    text). partial, if given, is (rows, table) for cells nothing matches: the
    same lookup on just their top rows."""
    cw, ch = width * scale_x, height * scale_y
    cells = []
    for r in range(rows):
        y0 = off_y + r * ch
        band = [img.row_bytes(y0 + gy * scale_y, off_x, off_x + cols * cw) for gy in range(height)]
        if band[0] == band[0][:3] * (cols * cw) and all(line == band[0] for line in band):
            rgb = (band[0][0] << 16) | (band[0][1] << 8) | band[0][2]
            cells += [Cell(' ', 0x20, None, rgb) for _ in range(cols)]
            continue
        for c in range(cols):
            x0 = c * cw * 3
            prow = [[line[x0 + gx * scale_x * 3:x0 + gx * scale_x * 3 + 3] for gx in range(width)]
                    for line in band]
            counts = {}
            for line in prow:
                for p in line:
                    counts[p] = counts.get(p, 0) + 1
            if len(counts) == 1:
                (only,) = counts
                cells.append(Cell(' ', 0x20, None, (only[0] << 16) | (only[1] << 8) | only[2]))
                continue
            best = None
            best_partial = None
            for colour in counts:
                key = tuple(sum(1 << (width - 1 - gx) for gx in range(width) if line[gx] == colour)
                            for line in prow)
                code = table.get(key)
                lit = counts[colour]
                if code is not None:
                    if best is None or lit > best[0]:
                        best = (lit, code, colour)
                elif partial is not None:
                    # A cursor sits where text is about to go, mostly on a
                    # blank: the reading lighting fewer dots is the likelier.
                    code = partial[1].get(key[:partial[0]])
                    if code is not None and (best_partial is None or lit < best_partial[0]):
                        best_partial = (lit, code, colour)
            best = best or best_partial
            if best is None:
                cells.append(Cell('\ufffd', None, None, None))
                continue
            _, code, colour = best
            fg = (colour[0] << 16) | (colour[1] << 8) | colour[2]
            others = [o for o in counts if o != colour]
            bg = None
            if len(others) == 1:
                o = others[0]
                bg = (o[0] << 16) | (o[1] << 8) | o[2]
            cells.append(Cell(char(code), code, fg, bg))
    return TextScreen(cells, cols, rows)


EFI_LZMA_GUID = bytes.fromhex('98584eee14395942' '9d6edc7bd79403cf')
EFI_GLYPH_WIDTH = 8
EFI_GLYPH_HEIGHT = 19
# The rows edk2's text cursor inverts.
EFI_CURSOR_ROWS = 16
_efi_fonts = {}


def efi_volumes(blob):
    """blob and, recursively, what its LZMA compressed sections hold."""
    yield blob
    i = 0
    while True:
        i = blob.find(EFI_LZMA_GUID, i)
        if i < 0:
            return
        # A GUID defined section: 24 bit size and type 2, the GUID, then the
        # offset of its data.
        sec = i - 4
        size = int.from_bytes(blob[sec:sec + 3], 'little')
        if sec >= 0 and blob[sec + 3] == 0x02 and size > 24:
            offset = struct.unpack_from('<H', blob, i + 16)[0]
            try:
                yield from efi_volumes(lzma.decompress(blob[sec + offset:sec + size], format=lzma.FORMAT_ALONE))
            except lzma.LZMAError:
                pass
        i += 16


def efi_console_font(firmware):
    """The narrow glyphs the edk2 firmware's graphical console draws, read out
    of the firmware image: an array of EFI_NARROW_GLYPH (UEFI 2.11 section
    33.3.2.2), a UTF-16 code, an attribute byte and 19 rows each, from U+0020
    to a zero code."""
    if firmware not in _efi_fonts:
        with open(firmware, 'rb') as f:
            blob = f.read()
        excl = struct.pack('<HB', 0x21, 0) + bytes((0, 0, 0, 0x18, 0x3c, 0x3c, 0x3c, 0x18, 0x18, 0x18, 0x18,
                                                    0x18, 0, 0x18, 0x18, 0, 0, 0, 0))
        entry = 3 + EFI_GLYPH_HEIGHT
        for vol in efi_volumes(blob):
            i = vol.find(excl) - entry
            if i >= 0 and struct.unpack_from('<H', vol, i)[0] == 0x20:
                break
        else:
            raise RuntimeError(f'no edk2 console font in {firmware}')
        table = {}
        while True:
            code = struct.unpack_from('<H', vol, i)[0]
            if code == 0:
                break
            table.setdefault(tuple(vol[i + 3:i + entry]), chr(code))
            i += entry
        partial = {}
        for rows, ch in table.items():
            partial.setdefault(rows[:EFI_CURSOR_ROWS], ch)
        _efi_fonts[firmware] = (table, partial)
    return _efi_fonts[firmware]


def decode_efi_console(img, firmware):
    """Read text back off a screenshot of edk2's graphical text console. Its
    text mode is centred on the screen; of the two QEMU's firmware uses, the
    largest that fits and the 80x25 every console has, the one reading as more
    text than noise wins (a boot logo reads as noise in either)."""
    table, partial = efi_console_font(firmware)
    best = None
    for cols, rows in ((img.width // EFI_GLYPH_WIDTH, img.height // EFI_GLYPH_HEIGHT), (80, 25)):
        off_x = (img.width - cols * EFI_GLYPH_WIDTH) >> 1
        off_y = (img.height - rows * EFI_GLYPH_HEIGHT) >> 1
        scr = decode_cells(img, table, lambda ch: ch, cols, rows, off_x, off_y, EFI_GLYPH_WIDTH,
                           EFI_GLYPH_HEIGHT, partial=(EFI_CURSOR_ROWS, partial))
        score = sum(-1 if cell.code is None else cell.ch != ' ' for cell in scr.cells)
        if best is None or score > best[0]:
            best = (score, scr)
    return best[1]


# The VGA 16 colour palette QEMU renders text mode with.
VGA_PALETTE = [
    0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa,
    0x555555, 0x5555ff, 0x55ff55, 0x55ffff, 0xff5555, 0xff55ff, 0xffff55, 0xffffff,
]


def decode_vga_text(mem, cols=80, rows=25):
    """Decode a dump of VGA text memory (character, attribute pairs). Limine
    turns blinking off, which makes attribute bit 7 background intensity."""
    cells = []
    for i in range(cols * rows):
        code = mem[i * 2]
        attr = mem[i * 2 + 1]
        cells.append(Cell(CP437[code] if code else ' ', code, VGA_PALETTE[attr & 15],
                          VGA_PALETTE[attr >> 4]))
    return TextScreen(cells, cols, rows)
