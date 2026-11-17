from harness.api import check
from harness.ports import ALL_PORTS
from cases._common import screen_text
from cases.menu import boot_menu, tree_menu, wait_menu

# Where the menu text sits and how its lines line up: interface_anchor,
# interface_align, and the two cell offsets. What the code alone decides is
# [intent]; the values themselves come from CONFIG.md.

NAMES = ('First', 'Collapsed', 'Last')


def placement_menu(globals_=''):
    return tree_menu('timeout: no\n' + globals_)


def entry_spans(scr, names=NAMES):
    """The cells a named entry's ' name ' covers, as (left, right, row)."""
    spans = []
    for name in names:
        hits = scr.find(f' {name} ')
        if not hits:
            return None
        x, y = hits[0]
        spans.append((x, x + len(name) + 1, y))
    return spans


def block(scr, names=NAMES):
    """The text block the named entries cover: leftmost and rightmost column,
    first and last row."""
    spans = entry_spans(scr, names)
    if spans is None:
        return None
    return min(s[0] for s in spans), min(s[2] for s in spans), max(s[1] for s in spans), max(s[2] for s in spans)


def require_block(t, m, desc):
    scr = wait_menu(t, m)
    got = block(scr)
    t.require(got is not None, f'{desc}: the entries are on screen')
    return scr, got


@check(ports=ALL_PORTS, tags=['menu'])
def menu_placement_anchor(t):
    """interface_anchor places the text block at an edge or a corner of the
    area left by the margins, and center_middle leaves it where it was."""
    _, bare = require_block(t, boot_menu(t, placement_menu()), 'default')
    _, centre = require_block(t, boot_menu(t, placement_menu('interface_anchor: center_middle\n')), 'center_middle')
    t.expect_eq(centre, bare, '[intent] center_middle is the default placement')

    scr, left_top = require_block(t, boot_menu(t, placement_menu('interface_anchor: left_top\n')), 'left_top')
    t.check(left_top[0] <= 4, f'left_top starts at the left edge ({left_top[0]})')
    t.check(left_top[1] < scr.rows // 4, f'left_top sits near the top ({left_top[1]} of {scr.rows} rows)')

    scr, right_bottom = require_block(t, boot_menu(t, placement_menu('interface_anchor: right_bottom\n')),
                                      'right_bottom')
    t.check(right_bottom[2] >= scr.cols - 8, f'right_bottom ends at the right edge ({right_bottom[2]} of {scr.cols})')
    t.check(right_bottom[3] > scr.rows // 2, f'right_bottom sits low ({right_bottom[3]} of {scr.rows} rows)')

    scr, middle = require_block(t, boot_menu(t, placement_menu('interface_anchor: middle\n')), 'middle')
    t.expect_eq((middle[0], middle[1]), (bare[0], bare[1]), '[intent] interface_anchor: middle keeps the default columns')
    t.check(abs((middle[0] + middle[2]) // 2 - scr.cols // 2) <= 3, 'a centred block is centred across the screen')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_placement_anchor_forms(t):
    """interface_anchor takes its two parts in either order and separated by
    '_', ',', or a space, and an unknown value keeps the default."""
    _, want = require_block(t, boot_menu(t, placement_menu('interface_anchor: left_top\n')), 'left_top')
    for value in ('top_left', 'left,top', 'top,left', 'left top', 'TOP_LEFT'):
        _, got = require_block(t, boot_menu(t, placement_menu(f'interface_anchor: {value}\n')), value)
        t.expect_eq(got, want, f'interface_anchor: {value} reads as left_top')

    _, bare = require_block(t, boot_menu(t, placement_menu()), 'default')
    _, got = require_block(t, boot_menu(t, placement_menu('interface_anchor: sideways_skywards\n')), 'unknown')
    t.expect_eq(got, bare, '[intent] an unknown interface_anchor keeps the default')

    for value in ('right_top', 'top_right', 'right,top'):
        _, want = require_block(t, boot_menu(t, placement_menu('interface_anchor: right_top\n')), 'right_top')
        _, got = require_block(t, boot_menu(t, placement_menu(f'interface_anchor: {value}\n')), value)
        t.expect_eq(got, want, f'interface_anchor: {value} reads as right_top')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_placement_align(t):
    """interface_align lines the entries up against each other: left, centre,
    or right, and right mirrors the tree decorations so they branch outwards."""
    def run(value=None):
        extra = 'interface_anchor: left_top\n'
        if value is not None:
            extra += f'interface_align: {value}\n'
        return require_block(t, boot_menu(t, placement_menu(extra)), str(value))

    scr, left = run()
    starts = [s[0] for s in entry_spans(scr)]
    t.expect_eq(len(set(starts)), 1, '[intent] left (the default) starts every line at the same column')

    scr, centre = run('center')
    mids = [(s[0] + s[1]) // 2 for s in entry_spans(scr)]
    t.check(max(mids) - min(mids) <= 1, f'center centres every line ({mids})')

    scr, right = run('right')
    ends = [s[1] for s in entry_spans(scr)]
    t.check(max(ends) - min(ends) <= 1, f'right ends every line at the same column ({ends})')

    text = screen_text(scr)
    t.check('├' not in text and '└' not in text, '[intent] right aligned lines do not branch to the left')
    t.check('┤' in text or '┘' in text or '<--' in text, '[intent] right aligned lines keep the branch on the right')


@check(ports=ALL_PORTS, tags=['menu'])
def menu_placement_offsets(t):
    """interface_offset_x and interface_offset_y nudge the text block by that
    many cells, negative included, and stop at the edges of the screen."""
    def run(extra):
        return require_block(t, boot_menu(t, placement_menu('interface_anchor: left_top\n' + extra)), extra)

    scr, base = run('')
    _, moved = run('interface_offset_x: 5\n')
    t.expect_eq((moved[0], moved[1]), (base[0] + 5, base[1]), 'interface_offset_x: 5 moves the block 5 cells right')

    _, moved = run('interface_offset_y: 3\n')
    t.expect_eq((moved[0], moved[1]), (base[0], base[1] + 3), 'interface_offset_y: 3 moves the block 3 rows down')

    _, moved = run('interface_offset_x: 5\ninterface_offset_y: 3\n')
    t.expect_eq((moved[0], moved[1]), (base[0] + 5, base[1] + 3), 'both offsets apply together')

    _, moved = run('interface_offset_x: -100\n')
    t.expect_eq(moved[0], base[0], '[intent] a negative offset stops at the left edge')

    scr, moved = run('interface_offset_x: 1000\n')
    t.check(moved[2] >= scr.cols - 8, f'[intent] a large offset stops at the right edge ({moved[2]} of {scr.cols})')

    _, moved = run('interface_offset_y: -100\n')
    t.check(moved[1] <= base[1], '[intent] a negative row offset does not push the block past the top')
