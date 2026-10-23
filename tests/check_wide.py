#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Wide screens (fdtp/WIDE_NOTES.md): headless runs at 120x40 and 170x48 and on
emulated displays. Every screen gets the larger grid: the filer's list (more
rows, more lists), the frame and band to the right edge, the boxes in the
middle, the viewers and the novel editor. Generated files stay in BUILD.

    python3 -B tests/check_wide.py --build build [--png]

--png also writes every snapshot as PNG next to it (tools/ppm2png.py), to look at.
"""
import argparse
import importlib.util
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
VERSION = re.search(r'FDTP_VERSION "([^"]+)"', (ROOT / 'src' / 'version.h').read_text()).group(1)
BG, CYAN, WHITE, YELLOW, MAGENTA, BLACK = range(6)
NAVY, CYAN_RGB = (0, 0, 0x55), (0, 0xff, 0xff)
checks = 0
REFERENCE = []   # the 80x25 filer of the fixture, for the frame's ends


def require(value, message):
    global checks
    if not value:
        raise AssertionError(message)
    checks += 1


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ppm2png = load_tool('ppm2png')
office = load_tool('make_test_office')


class Snap:
    def __init__(self, cells_path):
        self.path = cells_path
        self.data = json.loads(cells_path.read_text())
        self.rows = self.data['rows']
        self.w, self.h, self.rgb = ppm2png.read_ppm(cells_path.with_suffix('.ppm'))
        self.cols, self.nrows = len(self.rows[0]), len(self.rows)
        self.gx = (self.w - self.cols * 12) // 2

    def text(self, row, start=0, end=None):
        end = self.cols if end is None else end
        return ''.join(chr(c[0]) for c in self.rows[row][start:end] if c[3] != 2)

    def cell(self, col, row):
        return self.rows[row][col]

    def px(self, x, y):
        i = (y * self.w + x) * 3
        return tuple(self.rgb[i:i + 3])

    def gpx(self, x, y):
        """A pixel of the grid (x from the grid's left edge)."""
        return self.px(self.gx + x, y)

    def boxes(self):
        return self.data['boxes']

    def separators(self):
        return [y for y in range(self.nrows) if int(self.data['separators']) >> y & 1]


class Runner:
    def __init__(self, build, png):
        self.build = build
        self.base = build / 'wide-check'
        self.png = png
        self.exe = build / ('fdtp-preview' if sys.platform == 'darwin' else 'fdtp')
        require(self.exe.exists(), f'{self.exe}: build it first (make -C fdtp)')
        home = self.base / 'home'
        home.mkdir(parents=True, exist_ok=True)
        self.env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(build / 'tmp'), TZ='UTC', HOME=str(home),
                        FDTP_RC_SYSTEM='', FDTP_RC_USER=str(self.base / 'none.fdtprc'), VISUAL='true', EDITOR='true')
        self.count = 0

    def run(self, name, keys, directory, args=(), env=None, expect=None):
        prefix = self.base / 'snaps' / name
        prefix.parent.mkdir(parents=True, exist_ok=True)
        for old in prefix.parent.glob(name + '-*'):
            old.unlink()
        script = self.base / 'snaps' / f'{name}.keys'
        script.write_text(keys + '\n')
        # Every run starts afresh (the editor remembers 縦/横 in ~/.fdtp).
        shutil.rmtree(self.base / 'home', ignore_errors=True)
        (self.base / 'home').mkdir(parents=True)
        command = [str(self.exe)] + ([] if sys.platform == 'darwin' else ['--headless'])
        command += list(args) + ['--script', str(script), '--out', str(prefix), '--time', '1700000000', str(directory)]
        r = subprocess.run(command, cwd=ROOT.parent, env=dict(self.env, **(env or {})), capture_output=True, text=True, timeout=60)
        require(r.returncode == 0, f'{name}: exit {r.returncode}: {r.stderr[-500:]}')
        snaps = [Snap(p) for p in sorted(prefix.parent.glob(name + '-*.cells'))]
        if expect is not None:
            require(len(snaps) == expect, f'{name}: {len(snaps)} snapshots, expected {expect}')
        if self.png:
            for s in snaps:
                ppm2png.convert(s.path.with_suffix('.ppm'))
        self.count += len(snaps)
        return snaps

    def status(self, args, directory):
        command = [str(self.exe)] + ([] if sys.platform == 'darwin' else ['--headless'])
        return subprocess.run(command + list(args) + ['--keys', 'SNAP', '--out', str(self.base / 'snaps' / 'status'), str(directory)],
                              cwd=ROOT.parent, env=self.env, capture_output=True, text=True, timeout=60).returncode


# ---- fixture --------------------------------------------------------------
def png_bytes(w, h, pixel):
    rows = b''.join(b'\0' + b''.join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def quadrant(x, y, w, h):
    return (255, 0, 0) if x < w // 2 and y < h // 2 else (0, 255, 0) if y < h // 2 else (0, 0, 255) if x < w // 2 else (255, 255, 0)


NOVEL_LONG = '吾輩は猫である。名前はまだ無い。' * 8


def fixture(base):
    d = base / 'files'
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    for i in range(5):
        (d / f'dir{i}').mkdir()
    for i in range(240):
        f = d / f'f{i:03d}.txt'
        f.write_bytes(b'x' * (i + 1))
        os.utime(f, (1700000000 + i * 60, 1700000000 + i * 60))
    (d / ('a-very-long-file-name-that-needs-the-wide-name-column-' + 'z' * 20 + '.txt')).write_text('long\n')
    lines = [f'{n + 1:04d} テキストの行 line {n + 1}' for n in range(120)]
    lines[3] = 'W' * 300
    (d / 'text.txt').write_text('\n'.join(lines) + '\n')
    (d / 'data.bin').write_bytes(bytes(range(256)) * 32)
    (d / 'pic.png').write_bytes(png_bytes(400, 300, lambda x, y: quadrant(x, y, 400, 300)))
    rows = [','.join(f'r{r}c{c}' for c in range(40)) for r in range(80)]
    (d / 'book.csv').write_text('\n'.join(rows) + '\n')
    body = ''.join(f'<w:p><w:r><w:t>段落 {i + 1} ' + 'あいうえお' * 30 + '</w:t></w:r></w:p>' for i in range(30))
    office.make_docx(d / 'doc.docx', f'{office.XML_DECL}{office.DOCX_ROOT}<w:body>{body}</w:body></w:document>')
    jpeg = None
    for candidate in sorted((base.parent).glob('slide-check*/image2.jpeg')):
        jpeg = candidate.read_bytes()
        break
    if jpeg:
        office.make_pptx(d / 'deck.pptx', jpeg)
    novel = [f'第{n + 1}行　漢字《かんじ》の行。' for n in range(70)]
    novel[0] = NOVEL_LONG
    (d / 'novel.txt').write_text('\n'.join(novel) + '\n')
    return d


# ---- checks -----------------------------------------------------------------
def check_grid(s, cols, rows, canvas=None, label=''):
    require(s.nrows == rows and all(len(r) == cols for r in s.rows), f'{label}: grid {s.cols}x{s.nrows}, expected {cols}x{rows}')
    w, h = canvas or (cols * 12, rows * 24)
    require((s.w, s.h) == (w, h), f'{label}: canvas {s.w}x{s.h}, expected {w}x{h}')
    for y in range(s.h):
        for x in list(range(0, s.gx)) + list(range(s.gx + cols * 12, s.w)):
            if s.px(x, y) != NAVY:
                raise AssertionError(f'{label}: margin pixel {x},{y} is {s.px(x, y)}')
    for y in range(rows * 24, s.h):
        require(all(s.px(x, y) == NAVY for x in range(0, s.w, 7)), f'{label}: navy under the grid at y {y}')


def divider(i, cols):
    x = cols - 80
    return 13 if i == 0 else 36 + x // 3 if i == 1 else 57 + 2 * x // 3


def check_frame(s, label):
    """The FD frame to the right edge: lines, dividers and mirrored corners."""
    fw = s.cols * 12
    right = fw - 7
    for y in range(18, 126):
        require(s.gpx(5, y) == CYAN_RGB and s.gpx(6, y) == CYAN_RGB, f'{label}: left line at y {y}')
        require(s.gpx(right - 1, y) == CYAN_RGB and s.gpx(right, y) == CYAN_RGB, f'{label}: right line at y {y}')
        if y not in (59, 60):   # the row 1 line meets it there
            require(s.gpx(right + 1, y) != CYAN_RGB and s.gpx(right - 2, y) != CYAN_RGB, f'{label}: right line 2 dots at y {y}')
    for x in range(12, fw - 12):
        require(s.gpx(x, 131) == CYAN_RGB and s.gpx(x, 132) == CYAN_RGB, f'{label}: bottom line at x {x}')
    for x in range(6, right + 1):
        require(s.gpx(x, 59) == CYAN_RGB and s.gpx(x, 60) == CYAN_RGB, f'{label}: divider line at x {x}')
    for i in range(3):
        x = divider(i, s.cols) * 12 + 6
        # Under the title or the date (no top line there) it starts at row 1.
        top = 24 if s.gpx(x, 11) != CYAN_RGB and s.gpx(x - 1, 11) != CYAN_RGB else 14
        require(all(s.gpx(x, y) == CYAN_RGB and s.gpx(x - 1, y) == CYAN_RGB for y in range(top, 58)), f'{label}: row 1 divider {i}')
        require(top == 14 or all(s.gpx(x, y) != CYAN_RGB for y in range(12, 24)), f'{label}: row 1 divider {i} clear of the title')
    # The top line reaches the right corner (the date ends 4 columns before).
    require(all(s.gpx(x, 11) == CYAN_RGB for x in range(fw - 40, fw - 12)), f'{label}: top line to the right corner')
    # The right end (corners, line ends) is the 80-column screen's, dot for dot;
    # only lines are there (the numbers end 2 columns before).
    if REFERENCE:
        for y in range(0, 136):
            for i in range(14):
                require(s.gpx(fw - 14 + i, y) == REFERENCE[0].gpx(960 - 14 + i, y), f'{label}: right end at {fw - 14 + i},{y} unlike 80 columns')
            for x in range(14):
                require(s.gpx(x, y) == REFERENCE[0].gpx(x, y), f'{label}: left end at {x},{y} unlike 80 columns')


def check_band(s, label, labels):
    band = s.text(s.nrows - 1)
    for word in labels:
        require(word in band, f'{label}: band has {word!r}: {band!r}')
    cyan = [x for x in range(s.cols) if s.cell(x, s.nrows - 1)[2] == CYAN]
    require(cyan and max(cyan) > s.cols * 0.85, f'{label}: band reaches the right ({max(cyan) if cyan else None})')


def check_filer(r, d, cols, rows):
    size = f'{cols}x{rows}'
    s = r.run(f'filer-{size}', 'SNAP 5 SNAP 1 SNAP 2 HOLD-SHIFT SNAP RELEASE-SHIFT PGDN SNAP', d, ['--size', size], expect=5)
    two, five, one, shift, page2 = s
    for snap, name in zip(s, ('two', 'five', 'one', 'shift', 'page2')):
        check_grid(snap, cols, rows, label=f'{size} {name}')
        check_frame(snap, f'{size} {name}')
    list_rows, lists = rows - 8, cols // 40
    per = list_rows * lists
    entries = 1 + 5 + 241 + 6 + (1 if (d / 'deck.pptx').exists() else 0)
    pages = (entries + per - 1) // per
    require(two.text(1, 2, 13).rstrip() == f'Page: 1/{pages:2d}', f'{size}: page line {two.text(1, 2, 13)!r} ({per} a page)')
    sw = cols // lists
    require(two.text(6, 2, 15).strip() == '..', f'{size}: .. first')
    last_row = 6 + list_rows - 1
    require(two.text(last_row, 2, 15).strip() != '' and two.text(last_row + 1).strip() == '', f'{size}: list down to row {last_row}')
    require(two.text(6, (lists - 1) * sw + 2, (lists - 1) * sw + 15).strip().startswith('f'), f'{size}: list {lists} has names')
    require(two.text(6, (lists - 1) * sw + 2 + sw - 40 + 23, (lists - 1) * sw + sw - 2).strip()[:6] == '23-11-',
            f'{size}: the date in the last list {two.text(6, (lists - 1) * sw, cols)!r}')
    check_band(two, f'{size} filer', ['Logdsk', 'eXec', 'Editor', 'Unpack'])
    require(two.text(0).find('FDTP Version ' + VERSION) > 0 and two.text(0, cols - 20).strip().startswith('23-11-14'), f'{size}: title and date')
    require(two.text(3, cols - 22, cols - 16) == 'Marked' and two.text(4, cols - 22, cols - 17) == 'Files', f'{size}: right of rows 3 4')
    lists5 = (cols + 1) // 16
    used5 = min(lists5, (entries + list_rows - 1) // list_rows)
    require(five.text(6, 2 + (used5 - 1) * 16, 15 + (used5 - 1) * 16).strip() != '' and (used5 == lists5 or five.text(6, 2 + used5 * 16, 15 + used5 * 16).strip() == ''), f'{size}: {used5} of {lists5} name lists used')
    require(five.text(1, 2, 13).rstrip() == f'Page: 1/{(entries + list_rows * lists5 - 1) // (list_rows * lists5):2d}', f'{size}: mode 5 page')
    long_row = [y for y in range(6, rows - 2) if 'a-very-long' in one.text(y)]
    if long_row:
        require('-' + 'z' * 20 + '.txt' in one.text(long_row[0]), f'{size}: mode 1 shows the long name whole')
    require(one.text(6, cols - 11, cols - 1) == 'drwxr-xr-x' or one.text(6, cols - 11, cols - 1).startswith('d'), f'{size}: mode 1 mode column at the right {one.text(6)!r}')
    check_band(shift, f'{size} shift', ['Attr', 'sHell', 'Pack'])
    require(page2.text(1, 2, 13).rstrip() == f'Page: 2/{pages:2d}', f'{size}: PgDn goes a page of {per}')


def box_frame(s, b, label):
    col, row, w, h = b[:4]
    x0, x1 = col * 12 + 5, (col + w - 1) * 12 + 5
    y0, y1 = row * 24 + 11, (row + h - 1) * 24 + 11
    for x in range(x0 + 8, x1 - 6):
        require(s.gpx(x, y0) == CYAN_RGB and s.gpx(x, y1) == CYAN_RGB, f'{label}: box top/bottom at x {x}')
    for y in range(y0 + 8, y1 - 6):
        require(s.gpx(x0, y) == CYAN_RGB and s.gpx(x1 + 1, y) == CYAN_RGB, f'{label}: box sides at y {y}')
    # The shadow: black ground (grey characters) right of the box and under it.
    def black(cx, cy):
        return sum(1 for y in range(cy * 24, cy * 24 + 24) for x in range(cx * 12, cx * 12 + 12) if s.gpx(x, y) == (0, 0, 0))
    require(black(col + w, row + 1) > 150 and black(col + w, row + h) > 150, f'{label}: shadow right')
    require(black(col + 1, row + h) > 150 and black(col + w // 2, row + h) > 150, f'{label}: shadow below')
    require(black(col + w + 1, row + 1) < 100 and black(col, row + h) < 100, f'{label}: one cell of shadow')


def check_boxes(r, d, cols, rows):
    size = f'{cols}x{rows}'
    s = r.run(f'boxes-{size}', 'q SNAP n DOWN DOWN DOWN DOWN DOWN DOWN DOWN c SNAP ESC i SNAP ESC s SNAP ESC', d, ['--size', size], expect=4)
    confirm, copy, info, sort = s
    for snap, name in zip(s, ('confirm', 'copy', 'info', 'sort')):
        require(len(snap.boxes()) == 1, f'{size} {name}: one box')
        col, row, w, h = snap.boxes()[0][:4]
        require(col == (cols - w) // 2, f'{size} {name}: box in the middle across ({col}, w {w})')
        middle = rows // 2 - h // 2 if name != 'copy' else 9 + (rows - 25) // 2
        require(row == min(middle, rows - 2 - h), f'{size} {name}: box row {row}, expected {middle}')
        box_frame(snap, snap.boxes()[0], f'{size} {name}')
    require('終了しますか' in confirm.text(confirm.boxes()[0][1] + 1), f'{size}: the confirm text')
    field_row = copy.boxes()[0][1] + 2
    require(copy.cell(copy.boxes()[0][0] + 2, field_row)[2] == WHITE, f'{size}: the white field')


def check_help(r, d, cols, rows):
    size = f'{cols}x{rows}'
    s = r.run(f'help-{size}', 'h SNAP', d, ['--size', size], expect=1)[0]
    check_grid(s, cols, rows, label=f'{size} help')
    # Revision 1 item 25: a box (check_help.py checks it in detail).
    col, row, w, h = s.boxes()[0][:4]
    require('キー一覧' in s.text(row) and 'Esc: 戻る' in s.text(row + h - 1) and s.data['widget'] == 7, f'{size}: help box title and hint')


def check_screen_frame(s, label, pixel=False):
    """Revision 1 item 29: the screen's frame, box 0 over rows 0..rows-3 and
    every column, its lines to the right edge (x cols*12-7, -6) and the bottom
    line on row rows-3, the message line and the band outside it."""
    cols, rows = s.cols, s.nrows
    require(s.boxes() and s.boxes()[0][:4] == [0, 0, cols, rows - 2] and s.boxes()[0][5] == 1, f'{label}: the frame {s.boxes()[:1]}')
    right, bottom = cols * 12 - 7, (rows - 3) * 24 + 11
    for y in range(18, bottom - 6, 7):
        require(s.gpx(5, y) == CYAN_RGB and s.gpx(6, y) == CYAN_RGB, f'{label}: frame left line at y {y}')
        require(s.gpx(right, y) == CYAN_RGB and s.gpx(right + 1, y) == CYAN_RGB, f'{label}: frame right line at y {y}')
    gaps = s.data.get('gaps', [[0, 0, 0, 0]])[0]
    for x in range(12, cols * 12 - 12, 5):
        if not gaps[2] * 12 <= x < gaps[3] * 12:
            require(s.gpx(x, bottom) == CYAN_RGB and s.gpx(x, bottom + 1) == CYAN_RGB, f'{label}: frame bottom line at x {x}')
        if not gaps[0] * 12 <= x < gaps[1] * 12:
            require(s.gpx(x, 11) == CYAN_RGB and s.gpx(x, 12) == CYAN_RGB, f'{label}: frame top line at x {x}')


def vlines(s):
    return sorted(l[0] for l in s.data.get('lines', []) if l[3] == 1 and l[4] == 0 and l[1] == 0 and l[2] == s.nrows - 3)


def check_text(r, d, cols, rows):
    size = f'{cols}x{rows}'
    # Cursor to text.txt: named jump (Shift+letter goes to the next name with it).
    s = r.run(f'text-{size}', 'C-s "text.txt" RET v SNAP "#" SNAP', d, ['--size', size], expect=2)
    numbered, plain = s
    check_grid(numbered, cols, rows, label=f'{size} text')
    check_screen_frame(numbered, f'{size} text')
    # Body rows 1..rows-4, numbers in columns 1..4, the line down column 5.
    require(numbered.text(1, 1, 5).strip() == '1', f'{size}: the first line number')
    last = numbered.text(rows - 4)
    require(numbered.text(rows - 4, 1, 5).strip() != '' or 'W' in last, f'{size}: text down to row {rows - 4}: {last!r}')
    require(vlines(numbered) == [5], f'{size}: the line after the numbers joins the frame {vlines(numbered)}')
    require(all(numbered.gpx(65, y) == CYAN_RGB for y in range(11, (rows - 3) * 24 + 12)), f'{size}: the line from the top to the bottom of the frame')
    # The 300-character line wraps at the body width.
    wrow = [y for y in range(1, rows - 3) if numbered.text(y, 6).startswith('W' * 10)]
    require(wrow and numbered.text(wrow[0], 6, cols - 1).rstrip() == 'W' * (cols - 7), f'{size}: wraps at {cols - 7} columns')
    require(plain.text(wrow[0], 1, cols - 1).rstrip() == 'W' * (cols - 2), f'{size}: without numbers {cols - 2} columns')
    require('text.txt' in numbered.text(0) and '行 1/' in numbered.text(rows - 3), f'{size}: the name in the top line, the place in the bottom line')
    check_band(numbered, f'{size} text', ['Top', 'Search', 'Quit'])


def check_hex(r, d, cols, rows):
    size = f'{cols}x{rows}'
    s = r.run(f'hex-{size}', 'C-s "data.bin" RET b SNAP', d, ['--size', size], expect=1)[0]
    check_grid(s, cols, rows, label=f'{size} hex')
    check_screen_frame(s, f'{size} hex')
    # Sixteen bytes a row whatever the width; rows 1..rows-4.
    require(s.text(rows - 4, 1, 9) == f'{(rows - 5) * 16:08X}', f'{size}: address on row {rows - 4}: {s.text(rows - 4, 1, 9)!r}')
    require(s.text(1, 11, 59).rstrip() == '00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F', f'{size}: 16 bytes a row')
    require(vlines(s) == [9, 60], f'{size}: the lines join the frame {vlines(s)}')
    check_band(s, f'{size} hex', ['Search', 'Quit'])


def check_image(r, d, cols, rows):
    size = f'{cols}x{rows}'
    s = r.run(f'image-{size}', 'C-s "pic.png" RET v SNAP', d, ['--size', size], expect=1)[0]
    w, h = cols * 12, rows * 24
    require((s.w, s.h) == (w, h), f'{size}: image canvas')
    check_screen_frame(s, f'{size} image', pixel=True)
    # The picture area is the frame's body: x 12.., y 24..(rows-3)*24.
    vw, vh = w - 24, (rows - 4) * 24
    left, top = 12 + (vw - 400) // 2, 24 + (vh - 300) // 2
    require(s.px(left + 50, top + 50) == (255, 0, 0) and s.px(left + 350, top + 250) == (255, 255, 0), f'{size}: the picture in the middle')
    require(s.px(left - 2, top + 50) == NAVY and s.px(left + 402, top + 50) == NAVY, f'{size}: navy beside it')
    require('pic.png' in s.text(0) and '戻る' in s.text(rows - 1), f'{size}: image title and band')


def check_sheet(r, d, cols, rows):
    size = f'{cols}x{rows}'
    s = r.run(f'sheet-{size}', 'C-s "book.csv" RET v SNAP', d, ['--size', size], expect=1)[0]
    check_grid(s, cols, rows, label=f'{size} sheet')
    check_screen_frame(s, f'{size} sheet')
    # Data rows 2..rows-4, row numbers from column 1.
    require(s.text(rows - 4, 1, 6).strip() == str(rows - 5), f'{size}: row numbers to {rows - 5}: {s.text(rows - 4, 1, 6)!r}')
    letters = s.text(1)
    require(letters.split()[-1] >= ('K' if cols < 150 else 'O'), f'{size}: columns across {letters.split()[-3:]}')
    require('Quit' in s.text(rows - 1) and s.text(rows - 2, 2, 6) == 'A1: ', f'{size}: band and the cell line')
    # The grid's dots reach the frame: under the last row from the left line,
    # a column's line down to the bottom line.
    edge = (rows - 3) * 24 - 1
    require(all(s.gpx(x, edge) == CYAN_RGB for x in range(7, 60)), f'{size}: grid under the last row from the frame')
    xs = [x for x in range(13, cols * 12 - 13) if s.gpx(x, (rows - 3) * 24 + 5) == CYAN_RGB]
    require(len(xs) >= 5, f'{size}: the grid\'s lines reach the bottom line {xs[:5]}')


def check_doc(r, d, cols, rows):
    size = f'{cols}x{rows}'
    s = r.run(f'doc-{size}', 'C-s "doc.docx" RET v SNAP', d, ['--size', size], expect=1)[0]
    check_grid(s, cols, rows, label=f'{size} docx')
    check_screen_frame(s, f'{size} docx')
    filled = [y for y in range(1, rows - 3) if s.text(y, 1, cols - 1).strip()]
    require(filled and filled[-1] == rows - 4, f'{size}: paragraphs down to row {rows - 4} ({filled[-3:]})')
    widths = [max([x + 1 for x in range(cols - 1) if s.cell(x, y)[0] != 32] or [0]) for y in filled]
    require(max(widths) > cols - 5 and max(widths) <= cols - 1, f'{size}: lines {max(widths)} of {cols} columns')
    require('Quit' in s.text(rows - 1) and '段落' in s.text(rows - 2), f'{size}: band and the item line')


def check_slide(r, d, cols, rows):
    if not (d / 'deck.pptx').exists():
        print('  pptx: skipped (no slide-check*/image2.jpeg; run check_slide.py once)')
        return
    size = f'{cols}x{rows}'
    s = r.run(f'slide-{size}', 'C-s "deck.pptx" RET v SNAP', d, ['--size', size], expect=1)[0]
    check_screen_frame(s, f'{size} slide', pixel=True)
    w, h = cols * 12, rows * 24
    view_w, view_h = w - 24, (rows - 4) * 24
    drawn = [x for x in range(12, w - 12, 4) if s.px(x, 24 + view_h // 2) != NAVY]
    require(drawn, f'{size}: a slide')
    sw = drawn[-1] - drawn[0]
    require(sw > min(view_w, view_h * 4 / 3) * 0.9, f'{size}: the slide fits the view ({sw} of {view_w}x{view_h})')
    require('deck.pptx' in s.text(0) and '戻る' in s.text(rows - 1), f'{size}: slide title and band')


def ink(s, x0, y0, x1, y1):
    return sum(1 for y in range(y0, y1) for x in range(x0, x1) if s.px(x, y) != NAVY)


def check_novel(r, d, cols, rows):
    size = f'{cols}x{rows}'
    s = r.run(f'novel-{size}', 'C-s "novel.txt" RET e SNAP C-t SNAP', d, ['--size', size], expect=2)
    horizontal, vertical = s
    w, h = cols * 12, rows * 24
    require((horizontal.w, horizontal.h) == (w, h), f'{size}: editor canvas')
    check_screen_frame(horizontal, f'{size} editor', pixel=True)
    check_screen_frame(vertical, f'{size} vertical editor', pixel=True)
    # The body of the frame: x 12 .. w-12, y 24 .. (rows-3)*24.
    bottom = (rows - 3) * 24
    h_rows, chars = (bottom - 24) // 36, (w - 24) // 24
    last_y = 24 + (h_rows - 1) * 36 + 12
    require(ink(horizontal, 12, last_y, w - 12, last_y + 24) > 50, f'{size}: text on row {h_rows} of the editor')
    require(ink(horizontal, 12, last_y + 36, w - 12, bottom) == 0, f'{size}: nothing under row {h_rows}')
    # The long first line: full rows of `chars` characters.
    require(ink(horizontal, 12 + (chars - 1) * 24, 36, 12 + chars * 24, 60) > 20, f'{size}: a row is {chars} characters wide')
    v_cols, v_chars = (w - 24) // 36, (bottom - 24) // 24
    left = w - 12 - v_cols * 36
    require(ink(vertical, left, 24, left + 24, bottom) > 50, f'{size}: text in column {v_cols} (x {left})')
    require(ink(vertical, 12, 24, left - 2, bottom) == 0, f'{size}: nothing left of column {v_cols}')
    # The first (long) line fills its column to the last character.
    col0 = w - 12 - 36
    require(ink(vertical, col0, 24 + (v_chars - 1) * 24, col0 + 24, 24 + v_chars * 24) > 10, f'{size}: {v_chars} characters a column')
    require('縦書き' in vertical.text(rows - 3) and '保存' in vertical.text(rows - 1), f'{size}: editor status and band')


def check_customizer(r, d, cols, rows):
    size = f'{cols}x{rows}'
    s = r.run(f'config-{size}', 'z SNAP', d, ['--size', size], expect=1)[0]
    check_grid(s, cols, rows, label=f'{size} customizer')
    check_screen_frame(s, f'{size} customizer')
    # Revision 1 item 27: in the frame (rows 0..rows-3), the list on rows 5..rows-6.
    require([b[:4] for b in s.boxes()] == [[0, 0, cols, rows - 2]], f'{size}: the customizer frame {s.boxes()}')
    names = [s.text(y, 2, 27).strip() for y in range(5, rows - 5)]
    require('SCREENSIZE' in names and 'EDITOR' in names and 'AWAKE_MINUTES' in names, f'{size}: the variables, SCREENSIZE too: {names[-3:]}')
    require('auto' in s.text(5 + names.index('SCREENSIZE')), f'{size}: SCREENSIZE is auto')
    require('Esc終わる' in s.text(rows - 1) and s.text(rows - 4).strip(), f'{size}: help and keys rows')


def check_resize(r, d):
    """DISPLAY:WxH in a script: as an X window resized, every screen follows."""
    s = r.run('resize', 'C-s "text.txt" RET SNAP DISPLAY:1920x1080 SNAP DISPLAY:1370x790 SNAP v SNAP DISPLAY:2560x1440 SNAP '
              'DISPLAY:1024x600 SNAP ESC SNAP', d, expect=7)
    pomera, hd, odd, text_odd, text_qhd, text_pomera, back = s
    check_grid(pomera, 80, 25, (1024, 600), 'resize: start')
    check_grid(hd, 160, 45, (1920, 1080), 'resize: 1920x1080')
    check_frame(hd, 'resize: 1920x1080')
    check_grid(odd, 114, 32, (1370, 790), 'resize: 1370x790')
    require(odd.gx == 1, 'resize: 1370 wide, the grid one dot in')
    check_frame(odd, 'resize: 1370x790')
    require(text_odd.text(28, 1, 5).strip(), 'resize: text on row 28 (the last of the body)')
    check_screen_frame(text_odd, 'resize: 1370x790 text')
    check_grid(text_qhd, 213, 60, (2560, 1440), 'resize: 2560x1440 text')
    require(text_qhd.text(56, 1, 5).strip() != '', 'resize: the text view takes the new rows')
    check_screen_frame(text_qhd, 'resize: 2560x1440 text')
    check_grid(text_pomera, 85, 25, (1024, 600), 'resize: back to the Pomera text')
    check_grid(back, 85, 25, (1024, 600), 'resize: back at 1024x600, the Pomera\'s 85x25')
    check_frame(back, 'resize: 1024x600')


def check_choice(r, d):
    """screen_choose and screen_parse at their boundaries (canvas.c)."""
    cases = [((1024, 600), (85, 25), (1024, 600)), ((800, 480), (85, 25), (1024, 600)), ((909, 533), (85, 25), (1024, 600)),
             ((1025, 600), (85, 25), (1025, 600)), ((1024, 601), (85, 25), (1024, 601)), ((1366, 768), (113, 32), (1366, 768)),
             ((1920, 1080), (160, 45), (1920, 1080)), ((3840, 2160), (160, 45), (1920, 1080)), ((7680, 4320), (160, 45), (1920, 1080)),
             ((5120, 1440), (213, 30), (2560, 720)), ((1920, 500), (160, 25), (1920, 600)), ((800, 1280), (80, 53), (960, 1280))]
    for (w, h), grid, canvas in cases:
        s = r.run(f'display-{w}x{h}', 'SNAP', d, ['--display', f'{w}x{h}'], expect=1)[0]
        check_grid(s, grid[0], grid[1], canvas, f'--display {w}x{h}')
    sizes = [('79x24', (80, 25)), ('80x25', (80, 25)), ('85x25', (85, 25)), ('321x91', (320, 90)), ('120X40', (120, 40)), ('auto', (80, 25)), ('320x90', (320, 90))]
    for text, grid in sizes:
        s = r.run(f'size-{text}', 'SNAP', d, ['--size', text], expect=1)[0]
        canvas = (1024, 600) if grid in ((80, 25), (85, 25)) else (grid[0] * 12, grid[1] * 24)
        check_grid(s, grid[0], grid[1], canvas, f'--size {text}')
    for bad in ('0x0', '120x', 'x40', '120x40x2', 'big', '-80x25'):
        require(r.status(['--size', bad], d) == 2, f'--size {bad} is refused')
    # SCREENSIZE in ~/.fdtprc; --size wins; a bad value is not taken.
    rc = r.base / 'screen.fdtprc'
    rc.write_text('SCREENSIZE=120x40\n')
    s = r.run('rc-size', 'SNAP', d, env={'FDTP_RC_USER': str(rc)}, expect=1)[0]
    check_grid(s, 120, 40, label='SCREENSIZE=120x40')
    s = r.run('rc-size-cli', 'SNAP', d, ['--size', '170x48'], env={'FDTP_RC_USER': str(rc)}, expect=1)[0]
    check_grid(s, 170, 48, label='--size over SCREENSIZE')
    rc.write_text('SCREENSIZE=wide\n')
    s = r.run('rc-size-bad', 'SNAP', d, env={'FDTP_RC_USER': str(rc)}, expect=1)[0]
    check_grid(s, 80, 25, (1024, 600), 'SCREENSIZE=wide is not taken')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--png', action='store_true')
    args = parser.parse_args()
    build = args.build.resolve()
    (build / 'tmp').mkdir(parents=True, exist_ok=True)
    r = Runner(build, args.png)
    d = fixture(r.base)
    REFERENCE.extend(r.run('reference-80x25', 'SNAP', d, expect=1))
    check_grid(REFERENCE[0], 80, 25, (1024, 600), '80x25')
    check_frame(REFERENCE[0], '80x25')
    for cols, rows in ((120, 40), (170, 48)):
        for check in (check_filer, check_boxes, check_help, check_text, check_hex, check_image, check_sheet, check_doc,
                      check_slide, check_novel, check_customizer):
            check(r, d, cols, rows)
        print(f'  {cols}x{rows}: ok')
    check_resize(r, d)
    check_choice(r, d)
    print(f'check_wide: {checks} checks passed, {r.count} snapshots in {r.base / "snaps"}')


if __name__ == '__main__':
    try:
        main()
    except AssertionError as e:
        print(f'FAIL: {e}')
        sys.exit(1)
