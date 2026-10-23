#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The help boxes and scrolling boxes (docs/FDTP_REVISION1.md item 25).

h (and ?) in the filer and F1 in the novel editor show a floating box: the
CYAN frame (title in the top line, the hint and where the view is in the
bottom line), sections with a yellow heading in a line, keys (CYAN) and
their descriptions (WHITE) in aligned columns. A box with more rows than fit
scrolls: ↑↓ one row, PgUp PgDn Space a box's height, Home End. An
information box that does not fit (the novel editor's list of changes)
scrolls the same way down to its last row. Run at 80x25, 85x25 (the Pomera)
and 160x45; generated files stay in BUILD.

    python3 -B tests/check_help.py --build build [--png]

--png also writes the snapshots as PNG (tools/ppm2png.py) to look at.
"""
import argparse
import importlib.util
import re
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('check_wide', Path(__file__).with_name('check_wide.py'))
wide = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wide)
require, Runner, CYAN_RGB = wide.require, wide.Runner, wide.CYAN_RGB
BG, CYAN, WHITE, YELLOW = 0, 1, 2, 3
WIDGET_INFO, WIDGET_HELP = 5, 7
SIZES = ((80, 25), (85, 25), (160, 45))


def c_items(path, name):
    """The rows of a WidgetHelpItem table in a C source: (key, text), key None
    for a heading (H("...") or {NULL, "..."})."""
    source = path.read_text(encoding='utf-8')
    body = source[source.index(name):]
    body = body[body.index('{') + 1:body.index('};')]
    string = r'"((?:[^"\\]|\\.)*)"'
    # Strings are marked for translation as N_("..."); read the literal inside.
    body = re.sub(r'N_\(\s*("(?:[^"\\]|\\.)*")\s*\)', r'\1', body)
    items = []
    for m in re.finditer(r'H\(' + string + r'\)|\{\s*NULL\s*,\s*' + string + r'\s*\}|\{\s*' + string + r'\s*,\s*' + string + r'\s*\}', body):
        unescape = lambda s: s.replace('\\\\', '\\').replace('\\"', '"')
        if m.group(1) is not None:
            items.append((None, unescape(m.group(1))))
        elif m.group(2) is not None:
            items.append((None, unescape(m.group(2))))
        else:
            items.append((unescape(m.group(3)), unescape(m.group(4))))
    return items


FILER_ITEMS = c_items(ROOT / 'src' / 'commands.c', 'help_items[]')
NOVEL_ITEMS = c_items(ROOT / 'src' / 'noveledit.c', 'help_items[]')


def fixture(base):
    d = base / 'files'
    d.mkdir(parents=True, exist_ok=True)
    (d / 'a.txt').write_text('本文\n', encoding='utf-8')
    # Forty lines the publisher's format changes (half-width to full-width).
    (d / 'b.txt').write_text(''.join(f'abc{i}\n' for i in range(40)), encoding='utf-8')
    return d


def fg_text(s, row, start, end, colours):
    return ''.join(chr(c[0]) for c in s.rows[row][start:end] if c[3] != 2 and c[1] in colours and c[0] != 32)


def frame_rows(s, box):
    col, row, w, h = box[:4]
    return col, row, w, h


def check_box(s, box, label):
    """Frame to the corners (gaps only where text is written over it), text
    inside the frame, the box and its shadow on the screen."""
    col, row, w, h = frame_rows(s, box)
    require(col >= 0 and row >= 1 and col + w + 1 <= s.cols and row + h + 1 <= s.nrows - 1,
            f'{label}: box {col},{row} {w}x{h} with its shadow on a {s.cols}x{s.nrows} screen (above the band)')
    require(col == (s.cols - w) // 2, f'{label}: box in the middle across')
    x0, x1 = col * 12 + 5, (col + w - 1) * 12 + 5
    y0, y1 = row * 24 + 11, (row + h - 1) * 24 + 11
    # Sides: the whole height between the corners.
    for y in range(y0 + 8, y1 - 6):
        require(s.gpx(x0, y) == CYAN_RGB and s.gpx(x0 + 1, y) == CYAN_RGB and s.gpx(x1, y) == CYAN_RGB and s.gpx(x1 + 1, y) == CYAN_RGB,
                f'{label}: box sides at y {y}')
    # Top and bottom: drawn in every cell without text, up to the corners.
    for frame_row, y in ((row, y0), (row + h - 1, y1)):
        cells = s.rows[frame_row]
        written = [i for i in range(col + 1, col + w - 1) if cells[i][0] != 32]
        gap = (min(written) - 1, max(written) + 2) if written else (0, 0)
        for x in range(x0 + 8, x1 - 6):
            inside_gap = gap[0] * 12 <= x < gap[1] * 12
            require((s.gpx(x, y) == CYAN_RGB) != inside_gap or (inside_gap and cells[x // 12][0] != 32),
                    f'{label}: frame row {frame_row} at x {x} (gap {gap})')
        if written:
            require(gap[0] > col + 1 and gap[1] < col + w - 1, f'{label}: text in row {frame_row} stays between the corners')
    # Text only between the frame columns, one blank column inside each.
    for y in range(row + 1, row + h - 1):
        for x in (col, col + 1, col + w - 2, col + w - 1):
            require(s.rows[y][x][0] == 32, f'{label}: column {x} row {y} beside the frame is blank ({chr(s.rows[y][x][0])})')
    return col, row, w, h


def column_starts(s, box):
    """The first column of each column of the help box: inside the left frame,
    and two columns after each vertical line between columns."""
    col, row, w, h = box[:4]
    lines = [c for c in range(col + 2, col + w - 2)
             if all(s.gpx(c * 12 + 5, y * 24 + 3) == CYAN_RGB for y in range(row + 1, row + h - 1))]
    return [col + 2] + [c + 2 for c in lines]


def find_items(snaps, items, label):
    """Every item is shown in some snapshot: the key (CYAN) at a column's
    start, its text (WHITE) two columns after the widest key, a heading
    (YELLOW) one column in; a continuation ("") with nothing in the key place."""
    key_w = max(wide_width(k) for k, _ in items if k)
    places = []
    for s in snaps:
        box = s.boxes()[-1]
        col, row, w, h = box[:4]
        for x in column_starts(s, box):
            for y in range(row + 1, row + h - 1):
                places.append((s, x, y))
    require(places, f'{label}: no box')
    for key, text in items:
        found = False
        for s, x, y in places:
            if key is None:
                found = s.text(y, x + 1, x + 1 + wide_width(text)) == text and s.cell(x + 1, y)[1] == YELLOW
            else:
                tx = x + key_w + 2
                found = (s.text(y, tx, tx + wide_width(text)) == text and s.cell(tx, y)[1] == WHITE and
                         s.text(y, x, x + key_w + 2) == key + ' ' * (key_w + 2 - wide_width(key)) and
                         (not key or s.cell(x, y)[1] == CYAN))
            if found:
                break
        require(found, f'{label}: {"heading" if key is None else "item"} {key!r} {text!r} shown in its place')


def column_tops(s, label):
    """At the top of the list (the first view): a blank row under the title,
    then every column begins with a heading (YELLOW); a column that goes on
    with the section of the column before begins with 「見出し（続き）」."""
    box = s.boxes()[-1]
    col, row, w, h = box[:4]
    require(not s.text(row + 1, col + 1, col + w - 1).strip(), f'{label}: a blank row under the title')
    starts = column_starts(s, box)
    headings = [t for _, t in FILER_ITEMS + NOVEL_ITEMS if _ is None]
    for x in starts:
        top = s.text(row + 2, x + 1, col + w - 2).split('  ')[0].strip()
        require(s.cell(x + 1, row + 2)[1] == YELLOW and (top in headings or (top.endswith('（続き）') and top[:-4] in headings)),
                f'{label}: column at {x} begins with a heading ({top!r})')
    return len(starts)


def screen_text(s, box):
    col, row, w, h = box[:4]
    return [s.text(y, col + 2, col + w - 2) for y in range(row + 1, row + h - 1)]


def check_filer(r, d, cols, rows, png):
    size = f'{cols}x{rows}'
    keys = 'h SNAP ' + 'DOWN SNAP ' + 'PGDN SNAP ' * 8 + 'UP SNAP HOME SNAP END SNAP SPC SNAP ? SNAP'
    snaps = r.run(f'help-{size}', keys, d, ['--size', size])
    first = snaps[0]
    require(first.data['widget'] == WIDGET_HELP and first.data['depth'] == 1, f'{size}: h opens the help box')
    require(len(first.boxes()) == 1, f'{size}: one box')
    box = first.boxes()[0]
    col, row, w, h = check_box(first, box, f'{size} help')
    title = first.text(row)
    require(f'FDTP キー一覧  Version {wide.VERSION}' in title, f'{size}: title in the top line {title!r}')
    tx = col + 3
    require(first.cell(tx, row)[1] == YELLOW, f'{size}: title yellow at the left of the top line')
    # Where the view is, from the bottom line.
    def where(s):
        bottom = s.text(s.boxes()[0][1] + s.boxes()[0][3] - 1)
        m = re.search(r'(\d+)-(\d+)/(\d+)', bottom)
        require('Esc: 戻る' in bottom, f'{size}: hint {bottom!r}')
        return tuple(int(v) for v in m.groups()) if m else None
    shown = where(first)
    visible = h - 2
    seen = set()
    for s in snaps[:-1]:
        require(s.data['widget'] == WIDGET_HELP, f'{size}: the box stays open while scrolling')
        b = s.boxes()[0]
        require(b[:4] == box[:4], f'{size}: the box keeps its place')
        check_box(s, b, f'{size} help {s.path.name}')
        for line in screen_text(s, b):
            seen.add(line)
    find_items(snaps[:-1], FILER_ITEMS, size)
    column_tops(first, size)
    if shown:
        total = shown[2]
        require(shown[:2] == (1, visible), f'{size}: first view {shown}')
        down = where(snaps[1])
        require(down == (2, visible + 1, total), f'{size}: ↓ scrolls one row {down}')
        require(screen_text(snaps[1], box)[:-1] == screen_text(first, box)[1:], f'{size}: ↓ moves the rows up by one')
        pages = [where(s) for s in snaps[2:10]]
        require(pages[0] == (2 + visible, 1 + 2 * visible, total) or pages[0][1] == total, f'{size}: PgDn a box height {pages[0]}')
        require(pages[-1][1] == total, f'{size}: PgDn reaches the end {pages[-1]}')
        up = where(snaps[10])
        require(up == (pages[-1][0] - 1, total - 1, total), f'{size}: ↑ one row back {up}')
        require(where(snaps[11])[0] == 1 and where(snaps[12])[1] == total, f'{size}: Home and End')
        require(where(snaps[13]) == where(snaps[12]), f'{size}: Space at the end stays')
    else:
        require(cols >= 160, f'{size}: the whole list fits without scrolling only on a wide screen')
        # 160x45: two columns; the second goes on with 「ファイル操作（続き）」
        # (whole sections do not fit in 2 x 40 rows).
        col, row = box[0], box[1]
        right = column_starts(first, box)
        require(len(right) == 2 and first.text(row + 2, right[1] + 1).startswith('ファイル操作（続き）'),
                f'{size}: the second column {first.text(row + 2, right[1] + 1)[:20]!r}')
    last = snaps[-1]
    require(last.data['widget'] == 0 and last.data['depth'] == 1 and 'Path' in ''.join(last.text(y) for y in range(4)),
            f'{size}: ? closes the help')
    if png:
        print(f'  {size}: {snaps[0].path.with_suffix(".png")}')
    return snaps


def wide_width(text):
    return sum(2 if ord(ch) > 0xff and not 0xff61 <= ord(ch) <= 0xff9f else 1 for ch in text)


def check_closers(r, d, cols, rows):
    size = f'{cols}x{rows}'
    for key in ('ESC', 'q', 'h', 'RET'):
        s = r.run(f'help-close-{size}-{key}', f'h {key} SNAP', d, ['--size', size], expect=1)[0]
        require(s.data['widget'] == 0 and s.data['depth'] == 1 and not s.data['quit'], f'{size}: {key} closes the help (and does nothing else)')
    s = r.run(f'help-ignore-{size}', 'h x SNAP', d, ['--size', size], expect=1)[0]
    require(s.data['widget'] == WIDGET_HELP, f'{size}: another key leaves the help open')


def check_novel(r, d, cols, rows):
    """F1 in the novel editor: the same box with its own items."""
    size = f'{cols}x{rows}'
    keys = 'DOWN e F1 SNAP ' + 'PGDN SNAP ' * 4 + 'ESC SNAP'
    snaps = r.run(f'novel-help-{size}', keys, d, ['--size', size])
    first = snaps[0]
    require(first.data['widget'] == WIDGET_HELP and first.data['depth'] == 2, f'{size}: F1 in the editor opens the help box')
    box = first.boxes()[-1]
    check_box(first, box, f'{size} novel help')
    require('小説エディタのキー' in first.text(box[1]), f'{size}: novel help title')
    find_items(snaps[:-1], NOVEL_ITEMS, f'{size} novel')
    column_tops(first, f'{size} novel')
    require(snaps[-1].data['widget'] == 0 and snaps[-1].data['depth'] == 2, f'{size}: Esc closes the help, the editor stays')
    return snaps


def check_info_scroll(r, d, cols, rows):
    """An information box with more rows than fit: the list of changes of
    出版社の書式に整形 on 40 lines (all 40 listed, then 記録)."""
    size = f'{cols}x{rows}'
    keys = 'DOWN DOWN e F2 RET "y" SNAP DOWN SNAP END SNAP UP SNAP x SNAP'
    snaps = r.run(f'info-scroll-{size}', keys, d, ['--size', size], env={'FDTP_FORMAT_LOG': str(r.base / 'format.log')})
    first, down, end, up, closed = snaps
    require(first.data['widget'] == WIDGET_INFO, f'{size}: the list of changes is an information box')
    box = first.boxes()[-1]
    col, row, w, h = check_box(first, box, f'{size} changes')
    visible = h - 4
    require(visible == rows - 7, f'{size}: the box uses the screen height ({visible} rows)')

    def where(s):
        b = s.boxes()[-1]
        m = re.search(r'↑↓ (\d+)-(\d+)/(\d+)', s.text(b[1] + b[3] - 1))
        require(m, f'{size}: where the view is in the bottom line {s.text(b[1] + b[3] - 1)!r}')
        return tuple(int(v) for v in m.groups())
    total = where(first)[2]
    require('（80 件）' in first.text(row + 1) and total >= 81 and where(first)[:2] == (1, visible),
            f'{size}: 80 changes (2 on each of 40 lines) and 記録 ({where(first)}, {first.text(row + 1)!r})')
    require(where(down) == (2, visible + 1, total), f'{size}: ↓ one row')
    require(where(end)[1] == total and where(up)[1] == total - 1, f'{size}: End to the last row, ↑ back one')
    body = [end.text(y, col + 2, col + w - 2) for y in range(row + 3, row + h - 1)]
    at = [i for i, line in enumerate(body) if line.startswith('記録')]
    require(at and '40行目' in body[at[0] - 1] and all(not line[:4].strip() for line in body[at[0] + 1:]),
            f'{size}: the last rows: 40行目, then 記録 (its path may wrap) {body[-4:]!r}')
    require(not any('ほか' in line for line in body), f'{size}: no 「ほか M 件」')
    require(closed.data['widget'] == 0 and closed.data['depth'] == 2, f'{size}: another key closes it')
    return snaps


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--png', action='store_true')
    args = parser.parse_args()
    build = args.build.resolve()
    (build / 'tmp').mkdir(parents=True, exist_ok=True)
    r = Runner(build, args.png)
    r.base = build / 'help-check'
    (r.base / 'home').mkdir(parents=True, exist_ok=True)
    r.env['HOME'] = str(r.base / 'home')
    d = fixture(r.base)
    require(len(FILER_ITEMS) > 60 and len(NOVEL_ITEMS) > 20, 'help tables read from the sources')
    for cols, rows in SIZES:
        check_filer(r, d, cols, rows, args.png)
        check_closers(r, d, cols, rows)
        check_novel(r, d, cols, rows)
        check_info_scroll(r, d, cols, rows)
        print(f'  {cols}x{rows}: ok')
    # Whole sections are tried first: at 160x52 (47 rows) they fit in two columns, so
    # no column begins with 「（続き）」.
    s = r.run('help-160x52', 'h SNAP', d, ['--size', '160x52'], expect=1)[0]
    box = s.boxes()[0]
    require('Esc: 戻る' in s.text(box[1] + box[3] - 1) and not re.search(r'\d+-\d+/\d+', s.text(box[1] + box[3] - 1)), '160x52: no scrolling')
    require(column_tops(s, '160x52') == 2, '160x52: two columns')
    require('（続き）' not in ''.join(s.text(y) for y in range(s.nrows)), '160x52: whole sections, no 「（続き）」')
    print('  160x52: ok')
    print(f'check_help: {wide.checks} checks passed, {r.count} snapshots in {r.base / "snaps"}')


if __name__ == '__main__':
    try:
        main()
    except AssertionError as e:
        print(f'FAIL: {e}')
        sys.exit(1)
