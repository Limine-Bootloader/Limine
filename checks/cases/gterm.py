import struct

from harness.api import check
from harness.ports import ALL_PORTS
from harness import images
from harness.screen import Font, builtin_font
from cases._common import screen_text
from cases.menu import boot_menu, entry_cells, menu_entry

# Limine's graphical terminal as CONFIG.md's interface and graphical terminal
# options shape it, read back off the screen dot by dot. [intent] marks what
# only Limine's or Flanterm's code says.


def menu(globals_=''):
    return ('timeout: no\n' + globals_ + menu_entry(1, 'Only', 'only', 'a comment')
            + menu_entry(1, 'Other', 'other'))


def font8():
    """The built-in font squashed to 8x8, each row the OR of two."""
    f = builtin_font()
    return bytes(f[c * 16 + 2 * i] | f[c * 16 + 2 * i + 1] for c in range(256) for i in range(8))


def font_shifted():
    """The built-in font one row lower: 8x16, but none of its glyphs."""
    f = builtin_font()
    return b''.join(b'\0' + f[c * 16:c * 16 + 15] for c in range(256))


@check(ports=ALL_PORTS, tags=['gterm'])
def gterm_colours(t):
    """term_foreground and term_background colour the text, the selected
    entry showing them swapped [intent]; term_palette sets the eight ANSI
    colours, of which comments are in cyan [intent]."""
    palette = '010203;aa0000;00aa00;aa5500;0000aa;aa00aa;123456;aaaaaa'
    m = boot_menu(t, menu(f'term_foreground: 40e0d0\nterm_background: 00203040\nterm_palette: {palette}\n'))
    scr = t.wait_text(m, 'ARROWS')
    other = entry_cells(scr, 'Other')
    t.expect_eq((other[1].fg, other[1].bg), (0x40e0d0, 0x203040), '[spec] an entry in the terminal colours')
    selected = entry_cells(scr, 'Only')
    t.expect_eq((selected[1].fg, selected[1].bg), (0x203040, 0x40e0d0), '[intent] the selected entry reversed')
    x, y = scr.find_one('a comment')
    t.expect_eq(scr.cell(x, y).fg, 0x123456, '[intent] the comment in the palette\'s cyan')


@check(ports=ALL_PORTS, tags=['gterm'])
def gterm_font_spacing(t):
    """term_font_spacing puts 0 to 8 dots between glyphs; above 8, or not a
    number, it is as if unset: 1."""
    for value, spacing in (('0', 0), ('4', 4), ('8', 8), ('9', 1), ('two', 1)):
        m = boot_menu(t, menu(f'term_font_spacing: {value}\n'))
        t.wait_text(m, 'ARROWS', font=Font(spacing=spacing))
        m.stop()


@check(ports=ALL_PORTS, tags=['gterm'])
def gterm_font_scale(t):
    """term_font_scale scales glyphs by 1 to 8 in each direction; any other
    value is as if unset, which below 2560x1440 is 1x1, and 2x2 from there."""
    for value, sx, sy in (('2x2', 2, 2), ('2x1', 2, 1), ('1x2', 1, 2), ('9x9', 1, 1), ('0x2', 1, 1),
                          ('big', 1, 1)):
        m = boot_menu(t, menu(f'term_font_scale: {value}\n'))
        t.wait_text(m, 'ARROWS', font=Font(scale_x=sx, scale_y=sy))
        m.stop()
    m = boot_menu(t, menu('interface_resolution: 2560x1440\n'))
    big = Font(scale_x=2, scale_y=2)
    t.wait_screen(m, lambda s: 'ARROWS' in screen_text(s), desc='the menu at either scale',
                  font=lambda img: big if (img.width, img.height) == (2560, 1440) else Font())
    img = m.screendump()
    if (img.width, img.height) != (2560, 1440):
        t.note(f'no 2560x1440 mode here ({img.width}x{img.height}): the default scale at it is not checked')


@check(ports=ALL_PORTS, tags=['gterm'])
def gterm_font_file(t):
    """term_font loads a font, of term_font_size if given; a width other
    than 8, a size without term_font, a missing file, or one too short for
    the size leaves the built-in font."""
    files = {'/boot/f8.bin': font8(), '/boot/shifted.bin': font_shifted()}
    for options, font in (('term_font: boot():/boot/shifted.bin\n', Font(data=font_shifted())),
                          ('term_font: boot():/boot/f8.bin\nterm_font_size: 8x8\n', Font(data=font8(), height=8)),
                          ('term_font: boot():/boot/f8.bin\nterm_font_size: 9x8\n', Font()),
                          ('term_font_size: 8x8\n', Font()),
                          ('term_font: boot():/boot/missing.bin\n', Font()),
                          ('term_font: boot():/boot/f8.bin\n', Font())):
        m = boot_menu(t, menu(options), files=files)
        t.wait_text(m, 'ARROWS', font=font)
        m.stop()


@check(ports=ALL_PORTS, tags=['gterm'])
def gterm_margin(t):
    """term_margin insets the terminal by that many dots."""
    m = boot_menu(t, menu('term_margin: 40\nterm_margin_gradient: 99\n'))
    t.wait_text(m, 'ARROWS', margin=40)


def solid(fmt, width, height, colour):
    if fmt == 'jpeg':
        return images.jpeg_grey(width, height, colour & 0xff)
    return getattr(images, fmt)(width, height, [colour] * (width * height))


def blank_cell_bg(scr):
    """The background of a cell with nothing drawn in it, mid-screen."""
    y = scr.rows - 1
    return scr.cell(scr.cols // 2, y).bg


@check(ports=ALL_PORTS, tags=['gterm'])
def gterm_wallpaper(t):
    """A wallpaper in any of the formats fills the screen; with one, the
    terminal is inset by 64 dots and its background is 80000000, black let
    half through, by default; term_background with no transparency covers it
    exactly."""
    for fmt, ext, colour in (('bmp', 'bmp', 0x3070b0), ('png', 'png', 0xb07030), ('qoi', 'qoi', 0x30b070),
                             ('jpeg', 'jpg', 0x606060)):
        m = boot_menu(t, menu(f'wallpaper: boot():/bg.{ext}\n'), files={f'/bg.{ext}': solid(fmt, 64, 40, colour)})
        scr = t.wait_text(m, 'ARROWS', margin=64)
        img = m.screendump()
        t.expect_eq(img.pixel(8, 8), colour, f'[spec] {fmt}: the wallpaper outside the terminal')
        half = sum(round(((colour >> s) & 0xff) * 0x80 / 0xff) << s for s in (16, 8, 0))
        t.expect_eq(blank_cell_bg(scr), half, f'[spec] {fmt}: the terminal lets 80/ff of the wallpaper '
                    'through black')
        m.stop()
    m = boot_menu(t, menu('wallpaper: boot():/bg.bmp\nterm_background: 00ffffff\nterm_foreground: 000000\n'),
                  files={'/bg.bmp': solid('bmp', 8, 8, 0x000000)})
    scr = t.wait_text(m, 'ARROWS', margin=64)
    t.expect_eq(blank_cell_bg(scr), 0xffffff, '[spec] term_background 00ffffff: opaque white over black')


def qoi_hash(px):
    return (px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64


@check(ports=ALL_PORTS, tags=['gterm', 'hostile'])
def gterm_wallpaper_qoi_chunks(t):
    """A QOI wallpaper of every kind of chunk, with channels wrapping around
    and the longest run, decodes as the QOI specification 1.0 has it, alpha
    left out [spec]; one cut short inside a chunk, or out of chunks before
    its last pixel, is no wallpaper [intent]."""
    a, alpha = (0x10, 0x20, 0x30, 0xff), (0x40, 0x50, 0x60, 0x80)
    chunks = [('rgba', *a), ('diff', 1, -1, 0), ('luma', 20, -5, 3), ('run', 3), ('rgb', 0, 0xff, 1),
              ('diff', -2, 1, -1), ('luma', -32, 7, -8), ('rgba', *alpha), ('index', qoi_hash(a)),
              ('index', qoi_hash(alpha)), ('index', 0), ('run', 62), ('rgb', 0x12, 0x34, 0x56), ('run', 20)]
    data, pixels = images.qoi_chunks(32, 3, chunks)
    see_through = 'term_background: ff000000\nterm_margin: 0\n'
    m = boot_menu(t, menu('wallpaper: boot():/w.qoi\nwallpaper_style: tiled\n' + see_through), files={'/w.qoi': data})
    t.wait_text(m, 'ARROWS')
    img = m.screendump()
    for i, colour in enumerate(pixels):
        t.expect_eq(img.pixel(i % 32, i // 32), colour, f'dot {i % 32},{i // 32}')
    m.stop()
    head, pad = b'qoif' + struct.pack('>IIBB', 4, 1, 4, 0), b'\0' * 7 + b'\1'
    for what, body in (('an RGBA chunk cut short', b'\xff\x10\x20'),
                       ('a LUMA chunk cut short', b'\xfe\x10\x20\x30\x80'),
                       ('out of chunks', b'\xfe\x10\x20\x30\xc1')):
        m = boot_menu(t, menu('wallpaper: boot():/w.qoi\n'), files={'/w.qoi': head + body + pad})
        t.wait_text(m, 'ARROWS')
        t.expect_eq(m.screendump().pixel(2, 2), 0, f'{what}: no wallpaper')
        m.stop()


PATTERN = [0xff0000, 0x00ff00, 0x0000ff, 0xffff00, 0x00ffff, 0xff00ff, 0x808080, 0x102030]


@check(ports=ALL_PORTS, tags=['gterm'])
def gterm_wallpaper_style(t):
    """wallpaper_style: tiled repeats the image from the top left corner,
    stretched (the default) scales it to the screen, and centered puts it in
    the middle on the backdrop colour."""
    see_through = 'term_background: ff000000\nterm_margin: 0\n'
    files = {'/p.bmp': images.bmp(4, 2, PATTERN)}
    for style in ('tiled', None, 'centered'):
        option = f'wallpaper_style: {style}\nbackdrop: 405060\n' if style else ''
        m = boot_menu(t, menu('wallpaper: boot():/p.bmp\n' + see_through + option), files=files)
        t.wait_text(m, 'ARROWS')
        img = m.screendump()
        w, h = img.width, img.height
        if style == 'tiled':
            for x, y in ((0, 0), (3, 1), (5, 0), (6, 3), (9, 2)):
                t.expect_eq(img.pixel(x, y), PATTERN[(y % 2) * 4 + x % 4], f'[spec] tiled: dot {x},{y}')
        elif style is None:
            for x, y in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1), (w // 4 + 1, 1)):
                t.expect_eq(img.pixel(x, y), PATTERN[(y * 2 // h) * 4 + x * 4 // w],
                            f'[spec] stretched: dot {x},{y}')
        else:
            for x, y in ((0, 0), (w - 1, h - 1)):
                t.expect_eq(img.pixel(x, y), 0x405060, f'[spec] centered: backdrop at {x},{y}')
            region = (w // 2 - 2, h // 2 - 1, w // 2 + 2, h // 2 + 1)
            t.check(set(img.colours(region)) & set(PATTERN), f'[spec] centered: the image in the middle')
        m.stop()


@check(ports=ALL_PORTS, tags=['gterm'])
def gterm_resolution(t):
    """interface_resolution picks the screen's resolution for the menu; one
    that is not available leaves Limine to pick another."""
    for res in ('800x600', '640x480'):
        m = boot_menu(t, menu(f'interface_resolution: {res}\n'))
        t.wait_text(m, 'ARROWS')
        img = m.screendump()
        t.expect_eq(f'{img.width}x{img.height}', res, '[spec] the requested resolution')
        m.stop()
    m = boot_menu(t, menu('interface_resolution: 1234x777\n'))
    t.wait_text(m, 'ARROWS')
    img = m.screendump()
    t.check((img.width, img.height) != (1234, 777), f'[spec] another resolution: {img.width}x{img.height}')


@check(ports=['bios'], tags=['gterm'])
def gterm_text_mode_ignores(t):
    """In text mode interface_resolution and the term_ options do nothing."""
    m = boot_menu(t, menu('graphics: no\ninterface_resolution: 800x600\nterm_font_scale: 2x2\n'
                          'term_margin: 40\n'))
    scr = t.wait_text(m, 'ARROWS')
    img = m.screendump()
    t.expect_eq((img.width, img.height), (720, 400), '[spec] still 80x25 text')
    t.check('Only' in screen_text(scr), 'the menu in text mode')


@check(ports=ALL_PORTS, tags=['gterm'])
def gterm_rotation(t):
    """interface_rotation turns the interface by 90, 180 or 270 degrees,
    clockwise [intent]."""
    for degrees in (90, 180, 270):
        m = boot_menu(t, menu(f'interface_rotation: {degrees}\n'))
        t.wait_text(m, 'ARROWS', rotate=360 - degrees)
        m.stop()
