import re

from harness.screen import Cell, TextScreen

# Colours are kept symbolic for the 16 ANSI ones ('c0'..'c15') and as 0xRRGGBB
# integers for direct RGB, so checks state exactly what was sent.
DEFAULT_FG = 'default'
DEFAULT_BG = 'default'


class Terminal:
    """Enough of a VT100/xterm to replay what Limine and edk2 send to a
    serial line."""

    def __init__(self, cols=200, rows=60):
        self.cols = cols
        self.rows = rows
        self.reset()
        self.utf8_buf = b''
        self.state = 'ground'
        self.params = ''
        self.raw = bytearray()

    def reset(self):
        self.grid = [[' '] * self.cols for _ in range(self.rows)]
        self.attr = [[(DEFAULT_FG, DEFAULT_BG, False)] * self.cols for _ in range(self.rows)]
        self.x = 0
        self.y = 0
        self.fg = DEFAULT_FG
        self.bg = DEFAULT_BG
        self.bold = False
        self.reverse = False
        self.saved = (0, 0)
        self.cursor_visible = True
        self.pending_wrap = False
        self.clears = 0

    def feed(self, data):
        if isinstance(data, str):
            data = data.encode('latin-1')
        self.raw += data
        for b in data:
            self._byte(b)
        return self

    def _byte(self, b):
        st = self.state
        if st == 'ground':
            if b == 0x1b:
                self.state = 'esc'
            elif b == 0x0d:
                self.x = 0
                self.pending_wrap = False
            elif b == 0x0a:
                self._linefeed()
            elif b == 0x08:
                if self.x > 0:
                    self.x -= 1
                self.pending_wrap = False
            elif b == 0x09:
                self.x = min(self.cols - 1, (self.x // 8 + 1) * 8)
            elif b == 0x07 or b == 0x00:
                pass
            elif b < 0x20:
                pass
            elif b < 0x80:
                self._put(chr(b))
            else:
                self.utf8_buf += bytes([b])
                try:
                    ch = self.utf8_buf.decode('utf-8')
                except UnicodeDecodeError:
                    if len(self.utf8_buf) >= 4:
                        self._put('�')
                        self.utf8_buf = b''
                    return
                self.utf8_buf = b''
                self._put(ch)
        elif st == 'esc':
            if b == ord('['):
                self.state = 'csi'
                self.params = ''
            elif b == ord('7'):
                self.saved = (self.x, self.y)
                self.state = 'ground'
            elif b == ord('8'):
                self.x, self.y = self.saved
                self.state = 'ground'
            elif b == ord('c'):
                self.reset()
                self.clears += 1
                self.state = 'ground'
            elif b == ord('D'):
                self._linefeed()
                self.state = 'ground'
            elif b == ord('E'):
                self.x = 0
                self._linefeed()
                self.state = 'ground'
            elif b == ord('M'):
                if self.y > 0:
                    self.y -= 1
                else:
                    self.grid.insert(0, [' '] * self.cols)
                    self.grid.pop()
                    self.attr.insert(0, [(DEFAULT_FG, DEFAULT_BG, False)] * self.cols)
                    self.attr.pop()
                self.state = 'ground'
            elif b in (ord('('), ord(')')):
                self.state = 'charset'
            else:
                self.state = 'ground'
        elif st == 'charset':
            self.state = 'ground'
        elif st == 'csi':
            if 0x30 <= b <= 0x3f:
                self.params += chr(b)
            elif 0x20 <= b <= 0x2f:
                self.params += chr(b)
            else:
                self._csi(chr(b))
                self.state = 'ground'

    def _linefeed(self):
        self.pending_wrap = False
        if self.y == self.rows - 1:
            self.grid.pop(0)
            self.grid.append([' '] * self.cols)
            self.attr.pop(0)
            self.attr.append([(DEFAULT_FG, DEFAULT_BG, False)] * self.cols)
        else:
            self.y += 1

    def _put(self, ch):
        if self.pending_wrap:
            self.x = 0
            self._linefeed()
        fg, bg = (self.bg, self.fg) if self.reverse else (self.fg, self.bg)
        self.grid[self.y][self.x] = ch
        self.attr[self.y][self.x] = (fg, bg, self.bold)
        if self.x == self.cols - 1:
            self.pending_wrap = True
        else:
            self.x += 1

    def _nums(self, default=0):
        p = self.params.lstrip('?=>')
        if p == '':
            return []
        out = []
        for part in p.split(';'):
            out.append(int(part) if part.isdigit() else default)
        return out

    def _arg(self, nums, i, default):
        if i < len(nums) and nums[i] != 0:
            return nums[i]
        return default

    def _csi(self, final):
        nums = self._nums()
        private = self.params[:1] in ('?', '=', '>')
        self.pending_wrap = False
        if final in 'Hf':
            self.y = min(self.rows - 1, self._arg(nums, 0, 1) - 1)
            self.x = min(self.cols - 1, self._arg(nums, 1, 1) - 1)
        elif final == 'A':
            self.y = max(0, self.y - self._arg(nums, 0, 1))
        elif final == 'B':
            self.y = min(self.rows - 1, self.y + self._arg(nums, 0, 1))
        elif final == 'C':
            self.x = min(self.cols - 1, self.x + self._arg(nums, 0, 1))
        elif final == 'D':
            self.x = max(0, self.x - self._arg(nums, 0, 1))
        elif final == 'G':
            self.x = min(self.cols - 1, self._arg(nums, 0, 1) - 1)
        elif final == 'd':
            self.y = min(self.rows - 1, self._arg(nums, 0, 1) - 1)
        elif final == 'J':
            mode = nums[0] if nums else 0
            if mode == 2 or mode == 3:
                for y in range(self.rows):
                    self._erase(y, 0, self.cols)
                self.clears += 1
            elif mode == 0:
                self._erase(self.y, self.x, self.cols)
                for y in range(self.y + 1, self.rows):
                    self._erase(y, 0, self.cols)
            elif mode == 1:
                for y in range(0, self.y):
                    self._erase(y, 0, self.cols)
                self._erase(self.y, 0, self.x + 1)
        elif final == 'K':
            mode = nums[0] if nums else 0
            if mode == 0:
                self._erase(self.y, self.x, self.cols)
            elif mode == 1:
                self._erase(self.y, 0, self.x + 1)
            else:
                self._erase(self.y, 0, self.cols)
        elif final == 'm':
            self._sgr(nums or [0])
        elif final == 's':
            self.saved = (self.x, self.y)
        elif final == 'u':
            self.x, self.y = self.saved
        elif final in 'hl' and private:
            if 25 in nums:
                self.cursor_visible = final == 'h'
        elif final == '@':
            n = self._arg(nums, 0, 1)
            row = self.grid[self.y]
            self.grid[self.y] = (row[:self.x] + [' '] * n + row[self.x:])[:self.cols]
            arow = self.attr[self.y]
            self.attr[self.y] = (arow[:self.x] + [(self.fg, self.bg, False)] * n + arow[self.x:])[:self.cols]
        elif final == 'P':
            n = self._arg(nums, 0, 1)
            row = self.grid[self.y]
            self.grid[self.y] = row[:self.x] + row[self.x + n:] + [' '] * n
            arow = self.attr[self.y]
            self.attr[self.y] = arow[:self.x] + arow[self.x + n:] + [(DEFAULT_FG, DEFAULT_BG, False)] * n

    def _erase(self, y, x0, x1):
        for x in range(x0, min(x1, self.cols)):
            self.grid[y][x] = ' '
            self.attr[y][x] = (self.fg if self.reverse else DEFAULT_FG,
                               self.bg if not self.reverse else self.fg, False)

    def _sgr(self, nums):
        i = 0
        while i < len(nums):
            n = nums[i]
            if n == 0:
                self.fg, self.bg, self.bold, self.reverse = DEFAULT_FG, DEFAULT_BG, False, False
            elif n == 1:
                self.bold = True
            elif n == 22:
                self.bold = False
            elif n == 7:
                self.reverse = True
            elif n == 27:
                self.reverse = False
            elif 30 <= n <= 37:
                self.fg = f'c{n - 30}'
            elif n == 39:
                self.fg = DEFAULT_FG
            elif 40 <= n <= 47:
                self.bg = f'c{n - 40}'
            elif n == 49:
                self.bg = DEFAULT_BG
            elif 90 <= n <= 97:
                self.fg = f'c{n - 90 + 8}'
            elif 100 <= n <= 107:
                self.bg = f'c{n - 100 + 8}'
            elif n in (38, 48) and i + 1 < len(nums):
                if nums[i + 1] == 2 and i + 4 < len(nums):
                    rgb = (nums[i + 2] << 16) | (nums[i + 3] << 8) | nums[i + 4]
                    if n == 38:
                        self.fg = rgb
                    else:
                        self.bg = rgb
                    i += 4
                elif nums[i + 1] == 5 and i + 2 < len(nums):
                    if n == 38:
                        self.fg = f'c{nums[i + 2]}'
                    else:
                        self.bg = f'c{nums[i + 2]}'
                    i += 2
            i += 1

    def screen(self):
        cells = []
        for y in range(self.rows):
            for x in range(self.cols):
                fg, bg, _ = self.attr[y][x]
                cells.append(Cell(self.grid[y][x], None, fg, bg))
        return TextScreen(cells, self.cols, self.rows)

    def text(self):
        return '\n'.join(''.join(r).rstrip() for r in self.grid).rstrip('\n')


# CSI sequences, character set designations, and other two byte escapes.
_ESCAPE = re.compile(rb'\x1b(?:\[[0-?]*[ -/]*[@-~]|[()].|[^\[()])', re.S)


def plain(data):
    """data as text without its escape sequences and carriage returns."""
    return _ESCAPE.sub(b'', bytes(data)).replace(b'\r', b'').decode('utf-8', 'replace')
