#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Stage 4 acceptance tests (tables): zipfile.c, csv/tsv and xlsx.

Builds tests/sheet_harness.c with the sources of mk/sheet.mk, makes the
fixtures with tools/make_test_office.py, opens them through the harness,
checks the SNAP cells, saves, and reads the saved files back with Python's
zipfile and xml.etree. Everything is written under build/.

    python3 -B tests/check_sheet.py --build build [--sanitize]
"""
import argparse
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('office', ROOT / 'tools/make_test_office.py')
office = importlib.util.module_from_spec(spec)
spec.loader.exec_module(office)
NS = {'m': office.NS_MAIN}
M = '{' + office.NS_MAIN + '}'
SUPPORT = ['i18n', 'app', 'widgets', 'keys', 'commands', 'canvas', 'cells', 'raster', 'font24', 'jis', 'doc_undo']
checks = 0


def require(condition, message):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)


def mk_sources():
    text = (ROOT / 'mk/sheet.mk').read_text()
    return re.search(r'^SHEET_SOURCES := (.*)$', text, re.M).group(1).split()


def build(build_dir, sanitize):
    (build_dir / 'tmp').mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(build_dir / 'tmp'))
    if not (build_dir / 'font24.inc').exists():
        subprocess.run([sys.executable, str(ROOT / 'tools/gen_font.py'), '--output', str(build_dir / 'font24.inc')], check=True, env=env)
    if not (build_dir / 'jis0208.inc').exists():
        subprocess.run([sys.executable, str(ROOT / 'tools/gen_jis.py'), '--output', str(build_dir / 'jis0208.inc')], check=True, env=env)
    # The harness compiles src/*.c directly, so keep the catalog in step with them.
    subprocess.run([sys.executable, str(ROOT / 'tools/gen_i18n.py'), '--output', str(build_dir / 'i18n_en.inc')], check=True, env=env)
    exe = build_dir / ('sheet-test-asan' if sanitize else 'sheet-test')
    sources = [str(ROOT / 'src' / (n + '.c')) for n in mk_sources() + SUPPORT] + [str(ROOT / 'tests/sheet_harness.c')]
    flags = ['-O1', '-g', '-std=c11', '-Wall', '-Wextra', '-Werror']
    if sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    libs = ['-lz', '-lexpat', '-lm']
    if sys.platform != 'darwin':
        libs = subprocess.run(['pkg-config', '--libs', 'zlib', 'expat'], capture_output=True, text=True, check=True).stdout.split() + ['-lm']
    subprocess.run([os.environ.get('CC', 'cc')] + flags + ['-I' + str(ROOT / 'src'), '-I' + str(build_dir)] + sources + ['-o', str(exe)] + libs,
                   check=True, env=env)
    return exe


class Harness:
    def __init__(self, exe):
        self.exe = exe

    def run(self, *args, expected=0, env=None):
        r = subprocess.run([str(self.exe), *map(str, args)], capture_output=True,
                           env={**os.environ, **(env or {}), 'FDTP_LANGUAGE': 'ja'})
        out, err = r.stdout.decode('utf-8', 'replace'), r.stderr.decode('utf-8', 'replace')
        require(r.returncode == expected, f'{args}: exit {r.returncode}, expected {expected}\n{out}\n{err}')
        require('runtime error' not in err and 'AddressSanitizer' not in err and 'LeakSanitizer' not in err, err)
        return out

    def open(self, table, script, prefix, expected=0, env=None):
        out = self.run('open', table, script, prefix, expected=expected, env=env)
        snaps = []
        for line in out.splitlines():
            if line.startswith('SNAP '):
                serial = int(line.split()[1])
                snaps.append(inside(json.loads(Path(f'{prefix}-{serial:03d}.cells').read_text())))
        return out, snaps


# Revision 1 item 29: the table in the screen's frame (rows 0..22). Rows 1..21
# (the column names and 20 data rows) are read from column 1, as if the frame
# were not there, so a column keeps its number from before (the row numbers'
# column from 0); rows 0 (the name), 22 (the frame's bottom line with the
# sheet or the code), 23 (the cell) and 24 (the band) are as drawn.
DATA_ROWS = 20


def inside(snap):
    for y in range(1, 22):
        snap['rows'][y] = snap['rows'][y][1:] + [[32, 2, 0, 0]]
    return snap


def status(snap):
    """The bottom line of the frame: the sheet and its number, or the code."""
    return row_text(snap, 22).strip()


def row_text(snap, y):
    return ''.join(chr(c[0]) for c in snap['rows'][y] if c[3] != 2).rstrip()


def cell_text(snap, col_x, width, y):
    cells = snap['rows'][y][col_x:col_x + width]
    return ''.join(chr(c[0]) for c in cells if c[3] != 2).strip()


def grid(snap, address, left=0, gutter=5, width=10, top=0):
    """Text of a cell on screen, for the default layout (columns from A)."""
    m = re.fullmatch(r'([A-Z]+)(\d+)', address)
    col = 0
    for ch in m.group(1):
        col = col * 26 + ord(ch) - 64
    col -= 1 + left
    row = int(m.group(2)) - 1 - top
    return cell_text(snap, gutter + col * width, width, 2 + row)


def status23(snap):
    return row_text(snap, 23).strip()


# ---- the grid (revision 1 §3 17) ----
GRID_X = 32 + 12                 # CANVAS_GRID_X and the frame's column
GROUND, LINE, WHITE_RGB = (0, 0, 0x55), (0, 0xff, 0xff), (0xff, 0xff, 0xff)


def read_ppm(path):
    data = Path(path).read_bytes()
    magic, w, h, depth, body = data.split(maxsplit=4)
    w, h = int(w), int(h)
    return w, body[len(body) - w * h * 3:]


def pixel(img, x, y):
    """Colour at grid pixel (x, y)."""
    w, body = img
    at = (y * w + GRID_X + x) * 3
    return tuple(body[at:at + 3])


def check_grid(label, ppm, snap, gutter=5, widths=(), skip_v=(), skip_h=(), nrows=DATA_ROWS, ncols=99):
    """One-dot FD-cyan lines on the right and bottom edge of every cell.

    widths: widths of the visible columns from the left one (default 10).
    skip_v: (row, column) whose right edge has no line (a merge, a value in
    the gap); row -1 is the column names. skip_h: (row, column) whose bottom
    edge has none. Rows and columns count on screen from 0. ncols: the
    number of columns when the sheet ends on screen (XFD).
    A line pixel is never the ground colour (a line, or a character or the
    cursor it leaves alone); off the lines no FD cyan appears in cells whose
    text and ground are not cyan."""
    img = read_ppm(ppm)
    spans, at = [], gutter
    while at < 78 and len(spans) < ncols:
        w = widths[len(spans)] if len(spans) < len(widths) else 10
        spans.append((at, at + w))
        at += w
    # Inside the frame: 78 columns (936 dots); a column cut by the frame has
    # no line of its own at the right (the frame's line is there).
    right = min(936, spans[-1][1] * 12)
    edges = [('gutter', gutter * 12)] + [(k, e * 12) for k, (_, e) in enumerate(spans) if e * 12 < 936]
    bottoms = [(r, (2 + r + 1) * 24) for r in range(-1, nrows)]
    vx = {x - 1 for _, x in edges}
    hy = {y - 1 for _, y in bottoms}
    bad = []
    for k, x in edges:
        for r in range(-1, nrows):
            ys = range((2 + r) * 24, (3 + r) * 24)
            if k != 'gutter' and (r, k) in skip_v:
                if any(pixel(img, x - 1, y) == LINE for y in ys if y not in hy):
                    bad.append(f'line at the right of {r},{k} (should be left out)')
            elif any(pixel(img, x - 1, y) == GROUND for y in ys):
                bad.append(f'no line at the right of {r},{k} (x {x - 1})')
    for r, y in bottoms:
        for x in range(right):
            col = x // 12
            k = next((i for i, (a, b) in enumerate(spans) if a <= col < b), None)
            if k is not None and (r, k) in skip_h:
                continue
            if pixel(img, x, y - 1) == GROUND:
                bad.append(f'no line under row {r} at x {x}')
                break
    for y in range(24, (2 + nrows) * 24):
        if y in hy:
            continue
        row = snap['rows'][y // 24]
        for x in range(right):
            if x in vx:
                continue
            fg, bg = row[x // 12][1], row[x // 12][2]
            if fg != 1 and bg != 1 and pixel(img, x, y) == LINE:
                bad.append(f'stray cyan at {x},{y}')
                break
    require(not bad, f'{label}: ' + '; '.join(bad[:8]))


# ---- zipfile.c ----
class Unseekable(io.RawIOBase):
    def __init__(self):
        self.data = bytearray()

    def writable(self):
        return True

    def write(self, b):
        self.data += b
        return len(b)


def check_zip(h, out):
    d = out / 'zip'
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    names = {'a.txt': b'hello\n' * 1000, 'dir/': b'', 'dir/b.bin': bytes(range(256)) * 50, '日本語.txt': 'かな'.encode(), 'empty': b''}
    plain = d / 'plain.zip'
    with zipfile.ZipFile(plain, 'w') as z:
        z.comment = b'archive comment'
        for i, (name, data) in enumerate(names.items()):
            info = zipfile.ZipInfo(name, date_time=(2020, 5, 6, 7, 8, 10))
            info.compress_type = zipfile.ZIP_STORED if i % 2 else zipfile.ZIP_DEFLATED
            info.comment = name.encode()
            z.writestr(info, data)
    listing = h.run('zip-list', plain).splitlines()
    require(listing[0] == 'ZIP64 0', listing[0])
    require([line.split('\t')[0] for line in listing[1:]] == list(names), listing)
    require(any(line.startswith('日本語.txt\t') and line.endswith('\t2048') for line in listing), 'UTF-8 flag of a non-ASCII name')
    for name, data in names.items():
        if name.endswith('/'):
            continue
        h.run('zip-cat', plain, name, d / 'cat.bin')
        require((d / 'cat.bin').read_bytes() == data, f'zip-cat {name}')
    # Data descriptors (written to an unseekable stream) and a prefix.
    stream = Unseekable()
    with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr('x.txt', b'x' * 5000)
        z.writestr('y.txt', b'y' * 7)
    desc = d / 'descriptor.zip'
    desc.write_bytes(b'#!prefix data\n' * 3 + bytes(stream.data))
    with zipfile.ZipFile(desc) as z:
        require(all(i.flag_bits & 8 for i in z.infolist()), 'python wrote data descriptors')
    h.run('zip-cat', desc, 'x.txt', d / 'cat.bin')
    require((d / 'cat.bin').read_bytes() == b'x' * 5000, 'descriptor entry read with a prefix before the archive')
    # Rewrite: replace (store), delete, add (deflate); the rest raw-identical.
    (d / 'new.txt').write_bytes('新しい\n'.encode() * 100)
    (d / 'small.txt').write_bytes(b'small')
    rewritten = d / 'rewritten.zip'
    h.run('zip-rewrite', plain, rewritten, 'put', 'a.txt', d / 'small.txt', 'store', 'del', 'empty', 'put', 'added/新.txt', d / 'new.txt', 'deflate')
    with zipfile.ZipFile(rewritten) as z, zipfile.ZipFile(plain) as o:
        require(z.testzip() is None, 'rewritten CRCs')
        require(z.namelist() == ['a.txt', 'dir/', 'dir/b.bin', '日本語.txt', 'added/新.txt'], z.namelist())
        require(z.read('a.txt') == b'small' and z.getinfo('a.txt').compress_type == zipfile.ZIP_STORED, 'replaced store entry')
        require(z.read('added/新.txt') == (d / 'new.txt').read_bytes() and z.getinfo('added/新.txt').compress_type == zipfile.ZIP_DEFLATED, 'added entry')
        require(z.getinfo('added/新.txt').flag_bits & 0x800, 'UTF-8 flag on a new non-ASCII name')
        require(z.comment == b'archive comment' and z.getinfo('dir/b.bin').comment == b'dir/b.bin', 'comments kept')
        require(z.getinfo('a.txt').comment == b'a.txt', 'a replaced entry keeps its comment')
        for name in ('dir/', 'dir/b.bin', '日本語.txt'):
            require(raw_entry(rewritten, z.getinfo(name)) == raw_entry(plain, o.getinfo(name)), f'raw copy of {name}')
    shutil.copy(desc, d / 'desc-copy.zip')
    h.run('zip-rewrite', d / 'desc-copy.zip', d / 'desc-copy.zip', 'put', 'z.txt', d / 'small.txt', 'deflate')
    with zipfile.ZipFile(d / 'desc-copy.zip') as z:
        require(z.testzip() is None and z.read('x.txt') == b'x' * 5000 and z.read('z.txt') == b'small', 'descriptors copied in place')
    # A new archive from nothing.
    h.run('zip-rewrite', '-', d / 'fresh.zip', 'put', 'only.txt', d / 'small.txt', 'deflate')
    with zipfile.ZipFile(d / 'fresh.zip') as z:
        require(z.namelist() == ['only.txt'] and z.read('only.txt') == b'small', 'new archive')
    # ZIP64: read, but refuse to write.
    big = d / 'zip64.zip'
    saved = zipfile.ZIP64_LIMIT, zipfile.ZIP_FILECOUNT_LIMIT
    zipfile.ZIP64_LIMIT, zipfile.ZIP_FILECOUNT_LIMIT = 1000, 3
    try:
        with zipfile.ZipFile(big, 'w', compression=zipfile.ZIP_DEFLATED) as z:
            for i in range(5):
                z.writestr(f'f{i}.txt', bytes([65 + i]) * (3000 + i))
    finally:
        zipfile.ZIP64_LIMIT, zipfile.ZIP_FILECOUNT_LIMIT = saved
    require(b'PK\x06\x06' in big.read_bytes(), 'fixture has a ZIP64 end record')
    listing = h.run('zip-list', big).splitlines()
    require(listing[0] == 'ZIP64 1' and len(listing) == 6, listing)
    h.run('zip-cat', big, 'f4.txt', d / 'cat.bin')
    require((d / 'cat.bin').read_bytes() == b'E' * 3004, 'ZIP64 entry read')
    out_text = h.run('zip-rewrite', big, d / 'zip64-out.zip', 'put', 'x', d / 'small.txt', 'store', expected=3)
    require('ZIP64' in out_text and not (d / 'zip64-out.zip').exists(), 'ZIP64 writing refused, nothing written')
    # Damage: CRC, truncation, garbage; never a crash.
    for name, word in (('日本語.txt', 'CRC'), ('dir/b.bin', '壊れています')):
        bad = bytearray(plain.read_bytes())
        with zipfile.ZipFile(plain) as z:
            info = z.getinfo(name)
        at = info.header_offset + 30 + len(info.filename.encode()) + len(info.extra) + 2
        bad[at] ^= 0xff
        (d / 'bad.zip').write_bytes(bad)
        text = h.run('zip-cat', d / 'bad.zip', name, d / 'cat.bin', expected=3)
        require(word in text, f'damaged {name}: {text}')
    (d / 'cut.zip').write_bytes(plain.read_bytes()[:-40])
    h.run('zip-list', d / 'cut.zip', expected=3)
    (d / 'junk.zip').write_bytes(b'PK\x03\x04' + bytes(range(256)) * 4)
    h.run('zip-list', d / 'junk.zip', expected=3)
    (d / 'tiny.zip').write_bytes(b'PK')
    h.run('zip-list', d / 'tiny.zip', expected=3)
    print('zipfile: list/read store+deflate, descriptors, prefix, rewrite, ZIP64 read, damage')


def raw_entry(path, info):
    data = path.read_bytes()
    at = info.header_offset
    nlen = int.from_bytes(data[at + 26:at + 28], 'little')
    elen = int.from_bytes(data[at + 28:at + 30], 'little')
    return data[at:at + 30 + nlen + elen + info.compress_size]


# ---- xlsx ----
def cells_of(xml):
    root = ET.fromstring(xml)
    rows = root.find('m:sheetData', NS)
    result = {}
    order = []
    for row in rows:
        order.append(int(row.get('r')))
        cols = []
        for c in row:
            result[c.get('r')] = c
            cols.append(re.fullmatch(r'([A-Z]+)\d+', c.get('r')).group(1))
        keys = [sum((ord(ch) - 64) * 26 ** i for i, ch in enumerate(reversed(col))) for col in cols]
        require(keys == sorted(keys) and len(set(keys)) == len(keys), f'cells in column order in row {row.get("r")}')
    require(order == sorted(order) and len(set(order)) == len(order), f'rows in order: {order}')
    return root, result


def describe(c):
    f = c.find('m:f', NS)
    v = c.find('m:v', NS)
    t = c.find('m:is/m:t', NS)
    return {'s': c.get('s'), 't': c.get('t'), 'f': None if f is None else (f.text, dict(f.attrib)),
            'v': None if v is None else v.text, 'is': None if t is None else t.text}


def raw_cell(xml, ref):
    m = re.search(rf'<c r="{ref}"[^>]*?(/>|>.*?</c>)'.encode(), xml)
    return m.group(0) if m else None


def check_xlsx(h, out):
    d = out / 'xlsx'
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    original = office.make_xlsx(d / 'original.xlsx')
    book = d / 'book.xlsx'
    shutil.copy(original, book)
    os.chmod(book, 0o640)
    _, s = h.open(book, ROOT / 'tests/sheet_xlsx_edit.keys', d / 'edit')
    require(len(s) == 17, f'{len(s)} snapshots')
    first = s[0]
    require('book.xlsx' in row_text(first, 0) and status(first) == '売上  1/3', (row_text(first, 0), status(first)))
    require(first['frame'] == 0, 'full screen without the FD frame')
    header = row_text(first, 1)
    require(header.split() == list('ABCDEFG'), header)   # H: 3 columns inside the frame, its name further right
    require(all(c[1] == 1 for c in first['rows'][1][15:80] if c[0] != 32), 'column names in CYAN')
    require(all(first['rows'][y][3][1] == 1 for y in range(3, 22)), 'row numbers in CYAN')
    require(first['rows'][1][9][2] == 1 and first['rows'][2][3][2] == 1, 'current column and row names on CYAN')
    require(cell_text(first, 0, 5, 2) == '1' and cell_text(first, 0, 5, 21) == '20', 'row numbers 1..20')
    expect = {'A1': '品名', 'B1': '数量', 'D1': '金額', 'A3': 'みかん', 'A4': '漢字', 'B2': '10', 'C2': '120.50',
              'D2': '1205', 'D3': '1600', 'D4': '1000', 'E2': '2024/01/01', 'E4': '12:00', 'B6': '35', 'C6': '133.50',
              'D6': '3805', 'E6': '1205', 'B7': '5', 'C7': '20', 'D7': '9', 'E7': '59', 'A8': 'TRUE', 'C8': '3.142',
              'D8': '25.60%', 'E8': 'A B', 'A9': 'x', 'C9': '3', 'B8': '', 'A5': ''}
    for ref, text in expect.items():
        require(grid(first, ref) == text, f'{ref}: {grid(first, ref)!r} != {text!r}')
    require(grid(first, 'E3') == '#########', 'a datetime wider than the column shows #')
    require(grid(first, 'E1') == '結合見出', 'merged: text of the top-left cell only, clipped')
    require(status23(first) == 'A1: 品名', status23(first))
    cursor = first['rows'][2][5]
    require(cursor[1] == 0 and cursor[2] == 2, 'cursor cell reversed')
    band = row_text(first, 24)
    require('Edit' in band and 'Save' in band and 'Quit' in band, band)
    require(first['rows'][24][3][2] == 3, 'band hot letter on yellow')
    # The grid: E1:F1 is merged and E2 (2024/01/01) fills the gap of its column.
    check_grid('xlsx grid', d / 'edit-001.ppm', first, skip_v={(0, 4), (1, 4)})
    img = read_ppm(d / 'edit-001.ppm')
    require(all(pixel(img, (5 + 10) * 12 - 1, y) == WHITE_RGB for y in range(48, 72)) and
            all(pixel(img, x, 71) == WHITE_RGB for x in range(60, 180)), 'the cursor (A1) is not crossed by its lines')
    require(all(pixel(img, (5 + 10) * 12 - 1, y) == LINE for y in range(72, 96)), 'the line below the cursor is there')
    require(status23(s[1]) == 'D2: =B2*C2', status23(s[1]))
    after = s[2]
    for ref, text in {'B2': '30', 'D2': '3615', 'B6': '55', 'D6': '6215', 'C7': '30', 'E7': '99', 'D7': '9', 'E6': '1205', 'C6': '133.50'}.items():
        require(grid(after, ref) == text, f'recalc {ref}: {grid(after, ref)!r} != {text!r}')
    require(row_text(after, 0).endswith('変更あり'), 'changed marker')
    require(grid(s[3], 'D3') == '1601' and grid(s[3], 'D4') == '1000' and status23(s[3]) == 'D3: =B3*C3+1', status23(s[3]))
    require(grid(s[4], 'D2') == '7' and grid(s[4], 'D6') == '2608', 'formula replaced by a number')
    require(grid(s[5], 'B9') == '55' and status23(s[5]) == 'B9: =SUM(B2:B4)', status23(s[5]))
    require(grid(s[6], 'A5') == 'ins' and grid(s[6], 'A12') == 'end' and grid(s[6], 'B10') == 'hello', 'new cells shown')
    require(s[7]['notice'] == 'この関数は計算できません' and grid(s[7], 'E6') == '=VLOOKUP(', 'unsupported function kept as text')
    require(grid(s[8], 'C8') == '' and status23(s[8]) == 'C8:', 'cleared')
    require(grid(s[9], 'E2') == '2024/03/15', 'date input')
    require(s[10]['notice'] == '結合されたセルは左上だけ直せます', s[10]['notice'])
    require(s[11]['notice'] == '保存しました（元のファイルは .bak に残しました）', s[11]['notice'])
    require(not row_text(s[11], 0).endswith('変更あり'), 'saved: no changed marker')
    require(status(s[12]) == '予定  2/3' and row_text(s[12], 0).endswith('変更あり'), (row_text(s[12], 0), status(s[12])))
    require(grid(s[12], 'B2') == '5' and grid(s[12], 'B3') == '6', 'sheet 2 recalculated')
    require(grid(s[13], 'B2') == '1' and status(s[13]) == '空  3/3' and row_text(s[13], 0).endswith('変更あり'), row_text(s[13], 0))
    require(grid(s[14], 'A5') == '無番地' and grid(s[14], 'B5') == '7', 'cells without r on sheet 2')
    require(s[15]['notice'] == '保存しました', s[15]['notice'])
    require(s[16]['depth'] == 0, 'Esc closes when everything is saved')
    require((d / 'book.xlsx.bak').read_bytes() == original.read_bytes(), '.bak is the file as opened')
    require(oct(book.stat().st_mode & 0o777) == oct(0o640), 'file mode kept')
    require(not [p for p in d.iterdir() if p.name.startswith('.book.xlsx.')], 'no temporary files left')

    with zipfile.ZipFile(book) as z, zipfile.ZipFile(original) as o:
        require(z.testzip() is None, 'saved CRCs')
        require(z.namelist() == o.namelist(), 'same entries in the same order')
        changed = {'xl/worksheets/sheet1.xml', 'xl/worksheets/sheet2.xml', 'xl/worksheets/sheet3.xml', 'xl/calcChain.xml'}
        for info in o.infolist():
            if info.filename in changed:
                continue
            new = z.getinfo(info.filename)
            require(z.read(info.filename) == o.read(info.filename), f'{info.filename} content')
            require((new.compress_type, new.CRC, new.date_time) == (info.compress_type, info.CRC, info.date_time), f'{info.filename} header')
            require(raw_entry(book, new) == raw_entry(original, info), f'{info.filename} bytes')
        old1, new1 = o.read('xl/worksheets/sheet1.xml'), z.read('xl/worksheets/sheet1.xml')
        _, before = cells_of(old1)
        root, now = cells_of(new1)
        require(root.find('m:dimension', NS).get('ref') == 'A1:F12', root.find('m:dimension', NS).get('ref'))
        want = {
            'B2': {'s': None, 't': None, 'f': None, 'v': '30', 'is': None},
            'D2': {'s': None, 't': None, 'f': None, 'v': '7', 'is': None},
            'D3': {'s': None, 't': None, 'f': ('B3*C3+1', {}), 'v': '1601', 'is': None},
            'D4': {'s': None, 't': None, 'f': ('B4*C4', {}), 'v': '1000', 'is': None},
            'B6': {'s': None, 't': None, 'f': ('SUM(B2:B4)', {}), 'v': '55', 'is': None},
            'D6': {'s': None, 't': None, 'f': ('SUM(D2:D4)', {}), 'v': '2608', 'is': None},
            'C7': {'s': None, 't': None, 'f': ('MAX(B2:B4)', {}), 'v': '30', 'is': None},
            'E7': {'s': None, 't': None, 'f': ('(B2+B3)*2-B4/5', {}), 'v': '99', 'is': None},
            'E6': {'s': None, 't': 'inlineStr', 'f': None, 'v': None, 'is': '=VLOOKUP(A2,A2:D4,4)'},
            'B9': {'s': None, 't': None, 'f': ('SUM(B2:B4)', {}), 'v': '55', 'is': None},
            'B10': {'s': None, 't': 'inlineStr', 'f': None, 'v': None, 'is': 'hello'},
            'A12': {'s': None, 't': 'inlineStr', 'f': None, 'v': None, 'is': 'end'},
            'A5': {'s': None, 't': 'inlineStr', 'f': None, 'v': None, 'is': 'ins'},
            'C8': {'s': '4', 't': None, 'f': None, 'v': None, 'is': None},
            'E2': {'s': '1', 't': None, 'f': None, 'v': '45366', 'is': None},
        }
        for ref, value in want.items():
            require(ref in now and describe(now[ref]) == value, f'{ref}: {describe(now[ref]) if ref in now else None}')
        require(set(now) == set(before) | {'B9', 'B10', 'A12', 'A5'}, sorted(set(now) ^ set(before)))
        for ref in before:
            if ref in want:
                continue
            require(raw_cell(new1, ref) == raw_cell(old1, ref), f'unchanged cell {ref} byte for byte')
        rows = {r.get('r'): r for r in root.find('m:sheetData', NS)}
        require(rows['9'].get('spans') is None and rows['2'].get('spans') == '1:6', 'spans removed only where cells were added')
        require(rows['10'].get('ht') == '30' and rows['10'].get('customHeight') == '1', 'empty row element kept its attributes')
        require(new1.split(b'<dimension')[0] == old1.split(b'<dimension')[0], 'bytes before <dimension> kept')
        require(new1.split(b'</sheetData>')[1] == old1.split(b'</sheetData>')[1], 'bytes after </sheetData> kept')
        between = lambda x: x.split(b'<dimension')[1].split(b'/>', 1)[1].split(b'<sheetData>')[0]
        require(between(new1) == between(old1), 'bytes between <dimension> and <sheetData> kept')
        require(raw_cell(new1, 'A1') == raw_cell(old1, 'A1') and b'<row r="1" spans="1:6" x14ac:dyDescent="0.25">' in new1, 'row 1 untouched')
        old2, new2 = o.read('xl/worksheets/sheet2.xml'), z.read('xl/worksheets/sheet2.xml')
        require(new2.startswith(b'\xef\xbb\xbf<?xml') and new2.count(b'\r\n') == old2.count(b'\r\n'), 'sheet 2: BOM and CRLF kept')
        require(raw_cell(new2, 'B3') == b'<c r="B3"><f>B2+1</f><v>6</v></c>' and raw_cell(new2, 'B2') == b'<c r="B2"><v>5</v></c>', raw_cell(new2, 'B3'))
        require(new2.replace(b'<v>5</v>', b'<v>1</v>').replace(b'<v>6</v>', b'<v>2</v>') == old2, 'sheet 2: only B2 and B3 changed')
        new3 = z.read('xl/worksheets/sheet3.xml').decode()
        require('<dimension ref="A1:B2"/><sheetData><row r="2"><c r="B2"><v>1</v></c></row></sheetData>' in new3, new3)
        chain = re.findall(r'<c [^>]*/>', z.read('xl/calcChain.xml').decode())
        require(chain == ['<c r="D3" i="1"/>', '<c r="D4"/>', '<c r="B6"/>', '<c r="C6"/>', '<c r="D6"/>', '<c r="B7" i="1"/>',
                          '<c r="C7"/>', '<c r="D7"/>', '<c r="E7"/>', '<c r="B3" i="2"/>'], chain)
        # Shared strings untouched (byte for byte, checked above).
        require(z.read('xl/sharedStrings.xml') == o.read('xl/sharedStrings.xml'), 'sharedStrings untouched')

    # Read the saved book back.
    _, v = h.open(book, ROOT / 'tests/sheet_xlsx_view.keys', d / 'view')
    for ref, text in {'B2': '30', 'D2': '7', 'D3': '1601', 'D6': '2608', 'B9': '55', 'E2': '2024/03/15', 'A5': 'ins', 'C8': ''}.items():
        require(grid(v[0], ref) == text, f'reloaded {ref}: {grid(v[0], ref)!r}')
    require(grid(v[0], 'E6').startswith('=VLOOKUP'), 'reloaded text')
    require(row_text(v[1], 1).split()[:2] == ['A', 'B'] and v[1]['rows'][1][5 + 6][0] == ord('A'), 'column A widened to 13')
    require(cell_text(v[1], 5, 13, 2) == '品名', 'wide column content')
    require(v[2]['rows'][1][5][0] == ord('A') and v[2]['rows'][1][7][0] == 32, 'column A at the minimum width 2')
    require(status23(v[3]).startswith('A21:') and cell_text(v[3], 0, 5, 2) == '21', 'PgDn moves a page (20 rows)')
    require(status23(v[4]).startswith('E12:'), status23(v[4]))  # F1 is s="0" only: not stored
    require(status(v[5]) == '予定  2/3', status(v[5]))
    asked = ''.join(row_text(v[6], y).strip() for y in range(1, 23))  # the confirmation box (revision 1 §7)
    require(v[6]['widget'] == 2 and '保存せずに戻りますか？ (Y/N)' in asked, asked)
    require(v[7]['depth'] == 1 and row_text(v[7], 0).endswith('変更あり'), 'n keeps the screen')
    require(v[8]['depth'] == 0, 'y discards and closes')
    # The grid follows the widths (A at 13, then at 2), the page and the sheet.
    check_grid('grid, A widened', d / 'view-002.ppm', v[1], widths=[13], skip_v={(0, 4), (1, 4)})
    check_grid('grid, A narrowed', d / 'view-003.ppm', v[2], widths=[2], skip_v={(0, 4), (1, 4)})
    check_grid('grid, next page', d / 'view-004.ppm', v[3], widths=[2])
    check_grid('grid, sheet 2', d / 'view-006.ppm', v[5])
    # No line inside the confirmation box: its message row has none at the column edges.
    img = read_ppm(d / 'view-007.ppm')
    y = next(y for y in range(2, 23) if '保存せずに' in row_text(v[6], y))
    cols = [x for x, c in enumerate(v[6]['rows'][y]) if c[0] != 32 and x >= 5]  # the message, not the row number
    inside = [e * 12 - 1 for e in range(5, 81, 10) if cols[0] * 12 < e * 12 - 1 < (cols[-1] + 1) * 12]
    require(inside and all(pixel(img, x, py) != LINE for x in inside for py in range(y * 24, y * 24 + 24)), 'no grid in the box')
    with zipfile.ZipFile(book) as z:
        require(b'changed' not in z.read('xl/worksheets/sheet2.xml'), 'discarded change not written')

    # A calcChain whose only entry goes: part, relationship and content type go too.
    chain_book = office.make_formula_only_chain(d / 'chain.xlsx')
    keep = d / 'chain-original.xlsx'
    shutil.copy(chain_book, keep)
    script = d / 'chain.keys'
    script.write_text('RIGHT SNAP "5" RET C-s SNAP\n')
    _, c = h.open(chain_book, script, d / 'chain')
    require(status23(c[0]) == 'B1: =A1*2' and c[1]['notice'].startswith('保存しました'), c[1]['notice'])
    with zipfile.ZipFile(chain_book) as z, zipfile.ZipFile(keep) as o:
        require(z.testzip() is None and 'xl/calcChain.xml' not in z.namelist(), z.namelist())
        require(set(o.namelist()) - set(z.namelist()) == {'xl/calcChain.xml'}, 'only calcChain removed')
        require(b'calcChain' not in z.read('xl/_rels/workbook.xml.rels') and b'calcChain' not in z.read('[Content_Types].xml'), 'references removed')
        require(z.read('xl/_rels/workbook.xml.rels') == re.sub(rb'<Relationship Id="rId7"[^>]*/>', b'', o.read('xl/_rels/workbook.xml.rels')), 'rels: only that element removed')
        require(z.read('[Content_Types].xml') == re.sub(rb'<Override PartName="/xl/calcChain.xml"[^>]*/>', b'', o.read('[Content_Types].xml')), 'types: only that element removed')
        ET.fromstring(z.read('xl/_rels/workbook.xml.rels'))
        ET.fromstring(z.read('[Content_Types].xml'))

    # Broken files: a notice, no crash, nothing written.
    (d / 'junk.xlsx').write_bytes(b'not a zip at all' * 10)
    out = h.run('open', d / 'junk.xlsx', ROOT / 'tests/sheet_errors.keys', d / 'junk', expected=3)
    require('表を開けません' in out, out)
    (d / 'cut.xlsx').write_bytes(original.read_bytes()[:len(original.read_bytes()) // 2])
    require('表を開けません' in h.run('open', d / 'cut.xlsx', ROOT / 'tests/sheet_errors.keys', d / 'cut', expected=3), 'truncated xlsx')
    bad = bytearray(original.read_bytes())
    with zipfile.ZipFile(original) as z:
        info = z.getinfo('xl/worksheets/sheet1.xml')
    at = info.header_offset + 30 + len(info.filename.encode()) + len(info.extra) + 20
    bad[at] ^= 0x55
    (d / 'crc.xlsx').write_bytes(bad)
    (d / 'look.keys').write_text('SNAP ESC\n')
    before = bad
    _, c = h.open(d / 'crc.xlsx', d / 'look.keys', d / 'crc')
    require(c[0]['notice'].startswith('「売上」を開けません: zip が壊れています（xl/worksheets/sheet1.xml'), c[0]['notice'])
    require(status(c[0]) == '予定  2/3', 'the next sheet is shown')
    require((d / 'crc.xlsx').read_bytes() == bytes(before), 'nothing written')
    broken_xml = [(n, (b.replace('</sheetData>', '') if n == 'xl/worksheets/sheet1.xml' else b)) for n, b in office.xlsx_parts()]
    office.package(d / 'badxml.xlsx', broken_xml)
    _, c = h.open(d / 'badxml.xlsx', d / 'look.keys', d / 'badxml')
    require('XML が壊れています' in c[0]['notice'], c[0]['notice'])
    one = [(n, (b[:len(b) // 2] if n.startswith('xl/worksheets/') else b)) for n, b in office.xlsx_parts()]
    office.package(d / 'nosheet.xlsx', one)
    out = h.run('open', d / 'nosheet.xlsx', ROOT / 'tests/sheet_errors.keys', d / 'nosheet', expected=3)
    require('表を開けません' in out and 'XML が壊れています' in out, out)
    print('xlsx: view, edit, recalc, save (splice), calcChain, reload, errors')


# ---- csv / tsv ----
def check_csv(h, out):
    d = out / 'csv'
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    office.make_csv(d)
    results = {}
    for name in office.CSV:
        _, s = h.open(d / name, ROOT / 'tests/sheet_csv_edit.keys', d / name.replace('.', '-'))
        require(len(s) == 6 and s[4]['notice'].startswith('保存しました') and s[5]['depth'] == 0, f'{name}: {[x["notice"] for x in s]}')
        results[name] = s
    s = results['utf8.csv']
    require(status(s[0]) == 'UTF-8 LF', status(s[0]))
    require(grid(s[0], 'A1') == '名前' and grid(s[0], 'B2') == '100' and grid(s[0], 'A3') == '引用,あり', 'utf8 cells')
    require(grid(s[0], 'B3') == '改行 あり' and grid(s[0], 'A4') == '二重"引用', 'quoted cells')
    require(s[0]['rows'][3][5 + 10 + 8][0] == ord('0'), 'numeric text right-aligned')
    require(grid(s[1], 'B2') == '250' and grid(s[2], 'C1') == 'new', 'edited')
    require((d / 'utf8.csv').read_bytes() == '名前,値,new\nりんご,250\n"引用,あり","改行\nあり"\n"二重""引用",\n'.encode(), (d / 'utf8.csv').read_bytes())
    require((d / 'utf8.csv.bak').read_bytes() == office.CSV['utf8.csv'], 'csv .bak')
    s = results['bom.csv']
    require(status(s[0]) == 'UTF-8 BOM CRLF', status(s[0]))
    require((d / 'bom.csv').read_bytes() == b'\xef\xbb\xbfa,b,new\r\n1,250', (d / 'bom.csv').read_bytes())
    s = results['sjis.csv']
    require(status(s[0]) == 'Shift_JIS CRLF', status(s[0]))
    require(grid(s[0], 'A1') == '品名' and grid(s[0], 'A3') == 'ﾊﾝｶｸ' and grid(s[0], 'B3') == '①', 'Shift_JIS cells')
    require((d / 'sjis.csv').read_bytes() == '品名,数量,new\r\nりんご,250\r\n"ﾊﾝｶｸ",①\r\n'.encode('cp932'), (d / 'sjis.csv').read_bytes())
    s = results['data.tsv']
    require(grid(s[0], 'B2') == '2', 'tsv cells')
    require((d / 'data.tsv').read_bytes() == b'x\ty\tnew\n1\t250\n', (d / 'data.tsv').read_bytes())
    # Quoting what needs it; a TSV comma needs none.
    (d / 'q.csv').write_bytes(b'a\n')
    (d / 'q.keys').write_text('RET C-u "x,\\"y\\"" RET RIGHT "p q" RET C-s ESC\n')
    h.open(d / 'q.csv', d / 'q.keys', d / 'q')
    require((d / 'q.csv').read_bytes() == b'"x,""y""",p q\n', (d / 'q.csv').read_bytes())
    (d / 'q.tsv').write_bytes(b'a\tb\n')
    h.open(d / 'q.tsv', d / 'q.keys', d / 'qt')
    require((d / 'q.tsv').read_bytes() == b'"x,""y"""\tp q\n', (d / 'q.tsv').read_bytes())
    # Empty lines, a trailing delimiter, a CR-only file and an empty file round-trip.
    (d / 'shape.csv').write_bytes(b'a,b,\n\n,c\n')
    (d / 'shape.keys').write_text('DOWN DOWN DOWN DOWN "z" RET C-s ESC\n')
    h.open(d / 'shape.csv', d / 'shape.keys', d / 'shape')
    require((d / 'shape.csv').read_bytes() == b'a,b,\n\n,c\n\nz\n', (d / 'shape.csv').read_bytes())
    (d / 'cr.csv').write_bytes(b'1,2\r3,4')
    (d / 'cr.keys').write_text('SNAP "9" RET C-s ESC\n')
    _, s = h.open(d / 'cr.csv', d / 'cr.keys', d / 'cr')
    require(status(s[0]).endswith('CR') and grid(s[0], 'A2') == '3', 'CR newlines')
    require((d / 'cr.csv').read_bytes() == b'9,2\r3,4', (d / 'cr.csv').read_bytes())
    (d / 'empty.csv').write_bytes(b'')
    (d / 'empty.keys').write_text('SNAP "1" RET C-s ESC\n')
    h.open(d / 'empty.csv', d / 'empty.keys', d / 'empty')
    require((d / 'empty.csv').read_bytes() == b'1', (d / 'empty.csv').read_bytes())
    # Unreadable bytes: shown with U+FFFD, refused for editing; UTF-16 refused.
    (d / 'broken.csv').write_bytes(b'a,b\n\x82\n\xff\xfe\x80,\x81\n')
    _, s = h.open(d / 'broken.csv', ROOT / 'tests/sheet_errors.keys', d / 'broken')
    require('文字コードを正しく読めない' in s[1]['notice'] and s[2]['depth'] == 0, s[1]['notice'])
    require((d / 'broken.csv').read_bytes() == b'a,b\n\x82\n\xff\xfe\x80,\x81\n', 'unreadable csv untouched')
    (d / 'utf16.csv').write_bytes('a,b\n'.encode('utf-16'))
    require('UTF-16' in h.run('open', d / 'utf16.csv', ROOT / 'tests/sheet_errors.keys', d / 'u16', expected=3), 'UTF-16 refused')
    # Through a symbolic link: the target changes, the link stays; no .bak when the setting is off.
    (d / 'target.csv').write_bytes(b'1,2\n')
    (d / 'link.csv').symlink_to('target.csv')
    (d / 'link.keys').write_text('"7" RET C-s ESC\n')
    h.open(d / 'link.csv', d / 'link.keys', d / 'link', env={'FDTP_SHEET_NOBACKUP': '1'})
    require((d / 'link.csv').is_symlink() and (d / 'target.csv').read_bytes() == b'7,2\n', 'symlink kept, target written')
    require(not (d / 'link.csv.bak').exists() and not (d / 'target.csv.bak').exists(), 'backup setting off')
    # The far corner: XFD1048576, a wider row-number column, no crash at the edges.
    (d / 'far.keys').write_text('C-g "xfd1048576" RET DOWN RIGHT SNAP "1" RET SNAP C-g "A1" RET LEFT UP SNAP C-g "B0" RET SNAP ESC y\n')
    _, s = h.open(d / 'empty.csv', d / 'far.keys', d / 'far')
    require(status23(s[0]) == 'XFD1048576:' and 'XFD' in row_text(s[0], 1) and cell_text(s[0], 0, 8, 21) == '1048576', status23(s[0]))
    require(status23(s[1]) == 'XFD1048576: 1' and status23(s[2]) == 'A1: 1', status23(s[2]))
    require(s[2]['rows'][2][8][2] == 2 and s[2]['rows'][2][7][2] != 2,
            'returning from empty distant rows keeps the expanded gutter')
    require(s[3]['notice'] == '番地が正しくありません', s[3]['notice'])
    check_grid('grid at XFD1048576', d / 'far-001.ppm', s[0], gutter=8, ncols=7)
    check_grid('grid after returning from XFD1048576', d / 'far-003.ppm', s[2], gutter=8)
    check_grid('csv grid', d / 'utf8-csv-001.ppm', results['utf8.csv'][0])
    # CSV row counts include empty trailing rows, which have no stored cells.
    (d / 'rows.csv').write_bytes(b'x\n' + b'\n' * 19999)
    (d / 'rows.keys').write_text('SNAP PGDN SNAP C-g "A20000" RET SNAP HOME SNAP ESC\n')
    _, s = h.open(d / 'rows.csv', d / 'rows.keys', d / 'rows')
    for i, snap in enumerate(s):
        require(snap['rows'][1][10][0] == ord('A') and snap['rows'][1][9][0] == 32,
                f'20,000 CSV rows keep a five-digit gutter: snapshot {i}')
    require(grid(s[0], 'A1', gutter=6) == 'x' and grid(s[3], 'A1', gutter=6) == 'x',
            'CSV starts at column 6 before and after scrolling')
    # A character reaching the bottom pixel row keeps it: the line goes round it.
    (d / 'tall.csv').write_bytes('│,x\n'.encode())
    (d / 'tall.keys').write_text('DOWN SNAP ESC\n')
    _, s = h.open(d / 'tall.csv', d / 'tall.keys', d / 'tall')
    check_grid('grid round a tall character', d / 'tall-001.ppm', s[0])
    img = read_ppm(d / 'tall-001.ppm')
    stroke = [x for x in range(60, 84) if pixel(img, x, 60) == WHITE_RGB]
    require(stroke and all(pixel(img, x, 71) == WHITE_RGB for x in stroke), f'│ keeps its bottom dots: {stroke}')
    require(pixel(img, 58, 71) == LINE and pixel(img, 100, 71) == LINE, 'line on both sides of the │')
    require(not [p for p in d.iterdir() if p.name.startswith('.')], 'no temporary files left')
    print('csv/tsv: encodings, BOM, newlines, quoting, shapes, unreadable bytes')


def check_formulas(h):
    shift = {('B3*C3', 1, 0): 'B4*C4', ('$A$1+A$1+$A1+A1', 2, 3): '$A$1+D$1+$A3+D3', ('SUM(A1:B2)', 1, 1): 'SUM(B2:C3)',
             ('SUM(A:A)+SUM(1:2)', 1, 1): 'SUM(B:B)+SUM(2:3)', ("'S 1'!A1+S2!B$2", 1, 1): "'S 1'!B2+S2!C$2",
             ('"A1"&A1', 0, 1): '"A1"&B1', ('LOG10(A1)', 1, 0): 'LOG10(A2)', ('T[A1]+A1', 0, 1): 'T[A1]+B1',
             ('A1', -1, 0): '(null)', ('XFD1', 0, 1): '(null)', ('A1048576', 1, 0): '(null)', ('$A1048576', 0, 1): '$A1048576'}
    for (text, dr, dc), want in shift.items():
        got = h.run('shift', text, dr, dc).strip()
        require(got == want, f'shift {text} by {dr},{dc}: {got} != {want}')
    check = {'SUM(A1:A3)': 0, ' (1+2) * 3 ': 0, 'average(a1:b2)/count(A:A)': 0, '-2^2+10%': 0, 'MIN(A1,B2:C3,4)': 0,
             'TRUE+1': 0, 'VLOOKUP(A1,B:C,2)': 2, '_xlfn.STDEV.S(A1:A3)': 2, 'IF(A1>1,1,2)': 2, 'A1="x"': 3, 'A1&B1': 3,
             'Sheet2!A1': 3, 'NAME1': 3, '{1,2}': 3, '#N/A': 3, 'SUM()': 1, '1+': 1, '(1': 1, 'SUM(1,': 1, 'XFE1': 3}
    for text, want in check.items():
        got = int(h.run('check', text).strip())
        require(got == want, f'check {text!r}: {got} != {want}')
    print('formulas: reference moves and syntax classes')


def check_large(h, out):
    """20 000 x 10 numbers and a SUM column: opening, the last cell, an edit
    with the whole column recalculated, and the save."""
    d = out / 'large'
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    rows = []
    for r in range(1, 20001):
        cells = ''.join(f'<c r="{chr(65 + c)}{r}"><v>{r * 10 + c}</v></c>' for c in range(9))
        cells += f'<c r="J{r}"><f>SUM(A{r}:I{r})+J{r - 1}</f><v>0</v></c>' if r > 1 else f'<c r="J1"><f>SUM(A1:I1)</f><v>0</v></c>'
        # A chain referring downwards, ending in A1.
        cells += f'<c r="K{r}"><f>K{r + 1}+1</f><v>0</v></c>' if r < 20000 else '<c r="K20000"><f>A1</f><v>0</v></c>'
        rows.append(f'<row r="{r}">{cells}</row>')
    sheet = f'{office.XML_DECL}{office.SHEET_HEAD}<dimension ref="A1:J20000"/><sheetData>{"".join(rows)}</sheetData></worksheet>'
    parts = [(n, sheet if n == 'xl/worksheets/sheet1.xml' else b) for n, b in office.xlsx_parts() if n != 'xl/calcChain.xml']
    big = office.package(d / 'large.xlsx', parts)
    (d / 'large.keys').write_text('SNAP PGDN SNAP END SNAP HOME SNAP "1" RET C-s SNAP\n')
    _, s = h.open(big, d / 'large.keys', d / 'large')
    require(status23(s[0]) == 'A1: 10', status23(s[0]))
    require(status23(s[1]).startswith('A21:'), status23(s[1]))
    for i in (0, 1, 3):
        require(s[i]['rows'][2][6][2] == 2 and s[i]['rows'][2][5][2] != 2,
                f'20,000 rows reserve five digits plus a gap before and after scrolling: snapshot {i}')
        require(grid(s[i], 'A1' if i != 1 else 'A21', gutter=6, top=0 if i != 1 else 20) == ('10' if i != 1 else '210'),
                f'column A starts at column 6: snapshot {i}')
    require(status23(s[2]) == 'K20000: =A1', status23(s[2]))
    require(s[2]['rows'][1][10][0] == ord('E'), 'the five-digit gutter is unchanged at the last row')
    require(s[4]['notice'].startswith('保存しました'), s[4]['notice'])
    with zipfile.ZipFile(big) as z:
        root, now = cells_of(z.read('xl/worksheets/sheet1.xml'))
    # A chain referring upwards: every J is recalculated from the new A1.
    total = sum(sum(r * 10 + c for c in range(9)) for r in range(1, 20001)) - 10 + 1
    require(now['A1'].find('m:v', NS).text == '1' and float(now['J20000'].find('m:v', NS).text) == total, now['J20000'].find('m:v', NS).text)
    require(now['K1'].find('m:v', NS).text == '20000' and now['K20000'].find('m:v', NS).text == '1', now['K1'].find('m:v', NS).text)
    # A cycle: the cells keep a value, a notice says so, nothing hangs.
    cyc = [(n, (f'{office.XML_DECL}{office.SHEET_HEAD}<sheetData><row r="1"><c r="A1"><f>B1+1</f><v>0</v></c>'
                '<c r="B1"><f>A1+1</f><v>0</v></c><c r="C1"><v>1</v></c><c r="D1"><f>C1*2</f><v>2</v></c></row></sheetData></worksheet>'
                if n == 'xl/worksheets/sheet1.xml' else b)) for n, b in office.xlsx_parts() if n != 'xl/calcChain.xml']
    office.package(d / 'cycle.xlsx', cyc)
    (d / 'cycle.keys').write_text('RIGHT RIGHT "5" RET SNAP\n')
    _, c = h.open(d / 'cycle.xlsx', d / 'cycle.keys', d / 'cycle')
    require(grid(c[0], 'D1') == '10' and c[0]['notice'] in ('', '循環する参照があります'), c[0]['notice'])
    (d / 'cycle2.keys').write_text('"=B1+C1" RET SNAP\n')
    _, c = h.open(d / 'cycle.xlsx', d / 'cycle2.keys', d / 'cycle2')
    require(c[0]['notice'] == '循環する参照があります', c[0]['notice'])
    print('large: 220 000 cells, 20 000-long chains up and down, cycles, save')


def cells_xml(data):
    """{ref: bytes of its <c> element} of a sheet's XML."""
    return {m.group(1).decode(): m.group(0) for m in re.finditer(rb'<c r="([A-Z]+\d+)"(?:[^>]*/>|.*?</c>)', data, re.S)}


def check_undo(h, out):
    """Revision 1 item 38: C-z / C-y in the table view. One cell's edit is one
    step with the cells it recalculated and the members of a shared formula;
    「変更あり」 against the saved state; UNDO_LEVELS; csv too."""
    d = out / 'undo'
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    original = office.make_xlsx(d / 'original.xlsx')
    book = d / 'book.xlsx'
    shutil.copy(original, book)
    _, s = h.open(book, ROOT / 'tests/sheet_undo.keys', d / 'u')
    require(len(s) == 23, f'{len(s)} snapshots')
    n = [x['notice'] for x in s]

    def dirty(snap):
        return row_text(snap, 0).endswith('変更あり')
    first = {r: grid(s[0], r) for r in ('B2', 'D2', 'B6', 'D6', 'C7', 'E7', 'D3', 'D4')}
    require(first == {'B2': '10', 'D2': '1205', 'B6': '35', 'D6': '3805', 'C7': '20', 'E7': '59', 'D3': '1600', 'D4': '1000'}, first)
    require(n[1] == '取り消す物はありません' and not dirty(s[1]), n[1])
    require(grid(s[2], 'D2') == '3615' and grid(s[2], 'D6') == '6215' and dirty(s[2]), 'edited and recalculated')
    require(n[3] == '取り消しました（B2）' and {r: grid(s[3], r) for r in first} == first and not dirty(s[3]), (n[3], {r: grid(s[3], r) for r in first}))
    b2 = s[3]['rows'][3][15]   # B2: row 3, columns 15..24 (row numbers 0..4)
    require(b2[1] == 0 and b2[2] == 2, f'the cursor back on B2 {b2}')
    require(n[4] == 'やり直しました（B2）' and grid(s[4], 'B2') == '30' and grid(s[4], 'E7') == '99' and dirty(s[4]), n[4])
    require(grid(s[5], 'D3') == '1601' and grid(s[5], 'D4') == '1000', 'master edited')
    require(n[6] == '取り消しました（D3）' and grid(s[6], 'D3') == '1600' and status23(s[7]) == 'D3: =B3*C3', (n[6], status23(s[7])))
    require(n[8].startswith('保存しました') and not dirty(s[8]), n[8])
    require(n[9] == '取り消しました（B2）' and grid(s[9], 'B2') == '10' and dirty(s[9]), 'undone after saving: changed')
    require(n[10] == '保存しました' and not dirty(s[10]), n[10])
    require(n[11] == 'やり直しました（B2）' and dirty(s[11]) and n[12] == '取り消しました（B2）' and not dirty(s[12]), (n[11], n[12]))
    require(status(s[13]) == '予定  2/3' and grid(s[13], 'B3') == '6' and dirty(s[13]), 'sheet 2 edited')
    require(status(s[14]) == '売上  1/3', status(s[14]))
    require(n[15] == '取り消しました（予定!B2）' and status(s[15]) == '予定  2/3' and grid(s[15], 'B2') == '1' and grid(s[15], 'B3') == '2'
            and not dirty(s[15]), (n[15], status(s[15])))
    require(n[16] == '取り消しました（Z2）' and status23(s[17]) == 'Z2:' and dirty(s[17]), (n[16], status23(s[17])))
    require(n[18] == 'やり直しました（Z2）' and status23(s[19]) == 'Z2: b', (n[18], status23(s[19])))
    require(n[20] == '取り消しました（Z1）' and status23(s[21]) == 'Z1:' and not dirty(s[21]), (n[20], status23(s[21])))
    require(s[22]['depth'] == 0, 'not changed: Esc closes')
    # The last save was of the state undone after a save: every part as in the original.
    with zipfile.ZipFile(book) as z, zipfile.ZipFile(original) as o:
        require(z.namelist() == o.namelist() and z.testzip() is None, 'entries')
        for name in o.namelist():
            require(z.read(name) == o.read(name), f'{name}: undone and saved, the same as the original')
    # The file of the first save (snapshot 9): B2 30 and what it recalculated;
    # the shared formula whose edit was undone, as the file had it.
    shutil.copy(original, d / 'first.xlsx')
    (d / 'first.keys').write_text('C-g "B2" RET "30" RET C-g "A1" RET C-z C-y C-g "D3" RET RET C-u "=B3*C3+1" RET C-z C-s SNAP\n')
    _, s = h.open(d / 'first.xlsx', d / 'first.keys', d / 'f')
    with zipfile.ZipFile(d / 'first.xlsx') as z, zipfile.ZipFile(original) as o:
        new, old = cells_xml(z.read('xl/worksheets/sheet1.xml')), cells_xml(o.read('xl/worksheets/sheet1.xml'))
        require(z.read('xl/calcChain.xml') == o.read('xl/calcChain.xml'), 'calcChain kept')
    require(new['D3'] == old['D3'] and new['D4'] == old['D4'], 'the shared formula undone: master and member as in the file')
    require(new['B2'] == b'<c r="B2"><v>30</v></c>' and b'<v>3615</v>' in new['D2'] and b'<v>6215</v>' in new['D6'], (new['B2'], new['D2']))
    require(all(new[r] == old[r] for r in old if r not in ('B2', 'D2', 'B6', 'D6', 'B7', 'C7', 'E7')), 'only B2 and what it recalculated changed')
    # UNDO_LEVELS=2: three edits, two steps kept.
    shutil.copy(original, d / 'levels.xlsx')
    (d / 'levels.keys').write_text('C-g "B2" RET "1" RET C-g "B3" RET "2" RET C-g "B4" RET "3" RET SNAP C-z C-z SNAP C-z SNAP C-y C-y C-y SNAP\n')
    _, s = h.open(d / 'levels.xlsx', d / 'levels.keys', d / 'l', env={'FDTP_UNDO_LEVELS': '2'})
    got = [[grid(x, r) for r in ('B2', 'B3', 'B4')] for x in s]
    require(got == [['1', '2', '3'], ['1', '20', '5'], ['1', '20', '5'], ['1', '2', '3']], got)
    require(s[1]['notice'] == '取り消しました（B3）' and s[2]['notice'] == '取り消す物はありません' and row_text(s[2], 0).endswith('変更あり'),
            [x['notice'] for x in s])
    require(s[3]['notice'] == 'やり直す物はありません', s[3]['notice'])
    # csv: undone, redone, saved; undone after the save and saved again: the original bytes.
    csv = d / 'u.csv'
    csv.write_bytes(b'a,b\n1,2\n')
    (d / 'csv.keys').write_text('RIGHT DOWN RET C-u "9" RET SNAP C-z SNAP C-y C-s SNAP C-z SNAP C-s SNAP ESC SNAP\n')
    _, s = h.open(csv, d / 'csv.keys', d / 'c', env={'FDTP_UNDO_LEVELS': '100'})
    require(grid(s[0], 'B2') == '9' and row_text(s[0], 0).endswith('変更あり'), 'csv edited')
    require(grid(s[1], 'B2') == '2' and not row_text(s[1], 0).endswith('変更あり') and s[1]['notice'] == '取り消しました（B2）', s[1]['notice'])
    require(s[2]['notice'].startswith('保存しました') and grid(s[2], 'B2') == '9', s[2]['notice'])
    require(grid(s[3], 'B2') == '2' and row_text(s[3], 0).endswith('変更あり'), 'csv undone after saving')
    require(s[4]['notice'] == '保存しました' and csv.read_bytes() == b'a,b\n1,2\n' and s[5]['depth'] == 0, csv.read_bytes())
    print('undo: xlsx and csv, recalculated cells, shared formula, saves, other sheet, UNDO_LEVELS')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--executable', type=Path)
    args = parser.parse_args()
    build_dir = args.build.resolve()
    exe = args.executable.resolve() if args.executable else build(build_dir, args.sanitize)
    out = build_dir / 'sheet-check'
    out.mkdir(parents=True, exist_ok=True)
    h = Harness(exe)
    check_formulas(h)
    check_zip(h, out)
    check_csv(h, out)
    check_xlsx(h, out)
    check_large(h, out)
    check_undo(h, out)
    print(f'stage 4 sheet checks passed: {checks} checks')


if __name__ == '__main__':
    main()
