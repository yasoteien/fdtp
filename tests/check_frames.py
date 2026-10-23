#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Revision 1 items 27 and 29: every screen and box drawn with FD's frame and
lines, and every full screen in the same frame (docs/FDTP_03 §7): FD's frame
over rows 0..rows-3 and every column, the title in its top line, the status
in its bottom line, the lines inside joined to it.

    python3 -B tests/check_frames.py --build build [--png]

Each screen is photographed at 80x25, 85x25 (the Pomera) and 160x45 (a
1920x1080 PC) and must show a frame: the top FD frame (filer), a full-screen
frame box, or a floating box with its frame. --png writes every snapshot as
PNG beside it (BUILD/frames-check/snaps), to look at.
"""
import argparse
from pathlib import Path
import shutil
import sys
import zipfile

from check_wide import Runner, fixture, require, check_frame, office
import check_wide

ROOT = Path(__file__).resolve().parents[1]
SIZES = ('80x25', '85x25', '160x45')
CYAN_RGB = (0, 255, 255)
YELLOW = 3

# name: (keys up to the screen, how its frame is drawn)
#   'filer'  the top FD frame (check_wide.check_frame)
#   'full'   the screen's frame (revision 1 item 29): box 0 over rows 0 ..
#            rows-3, the title in its top line, its lines inside joined to it
#   'box'    a floating box (frame and shadow) over the screen
#   'grid'   'full', and the table's ruled cells run to the frame's lines
#   'vnum'   'full', and the vertical editor's numbers' line across the frame
SCREENS = {
    'filer': ('', 'filer'),
    'help': ('h', 'box'),
    'customizer-variables': ('z', 'full'),
    'customizer-keys': ('z RIGHT', 'full'),
    'customizer-keys-alt': ('z RIGHT PGDN PGDN', 'full'),
    'customizer-keys-symbols': ('z RIGHT PGDN PGDN PGDN PGDN PGDN PGDN PGDN PGDN PGDN', 'full'),
    'customizer-keys-end': ('z RIGHT END', 'full'),
    'customizer-reserved': ('z RIGHT HOME DOWN DOWN DOWN DOWN RET', 'full'),
    'customizer-launch': ('z RIGHT RIGHT', 'full'),
    'customizer-input': ('z RET', 'full'),
    'customizer-confirm': ('z ESC', 'full'),
    'tree': ('t', 'full'),
    'attributes': ('C-s "text.txt" RET a', 'box'),
    'file-info': ('C-s "text.txt" RET i', 'box'),
    'filesystem-info': ('S-F2', 'box'),
    'archive': ('C-s "arc.zip" RET v', 'filer'),
    'pack': ('C-s "text.txt" RET p', 'box'),
    'text': ('C-s "text.txt" RET v', 'full'),
    'text-plain': ('C-s "text.txt" RET v "#"', 'full'),
    'text-codes': ('C-s "text.txt" RET v c', 'box'),
    'hex': ('C-s "data.bin" RET b', 'full'),
    'hex-word': ('C-s "data.bin" RET b w', 'full'),
    'sheet': ('C-s "book.csv" RET v', 'grid'),
    'docx': ('C-s "doc.docx" RET v', 'full'),
    'docx-break': ('C-s "break.docx" RET v', 'full'),
    'pptx': ('C-s "deck.pptx" RET v', 'full'),
    'image': ('C-s "pic.png" RET v', 'full'),
    'novel': ('C-s "novel.txt" RET e', 'full'),
    'novel-numbers': ('C-s "novel.txt" RET e C-l', 'full'),
    'novel-vertical': ('C-s "novel.txt" RET e C-t', 'full'),
    'novel-vertical-numbers': ('C-s "novel.txt" RET e C-t C-l', 'vnum'),
    'novel-help': ('C-s "novel.txt" RET e F1', 'box'),
    'novel-convert': ('C-s "novel.txt" RET e F2', 'box'),
    'confirm-delete': ('C-s "text.txt" RET d', 'box'),
    'confirm-quit': ('q', 'box'),
    'input-copy': ('C-s "text.txt" RET c', 'box'),
    'choice-sort': ('s', 'box'),
    'choice-binary': ('C-s "data.bin" RET v', 'box'),
    'error': ('l "/nonexistent-fdtp" RET', 'box'),
    'find-dir': ('C-f', 'full'),
    'find-dir-found': ('C-f "dir3" RET WAIT:0.5', 'full'),
    'copy-tree': ('C-s "text.txt" RET c F8', 'full'),
    'mark-find': ('*', 'box'),
    'path-input': ('l', 'box'),
    'sysinfo': ('M-i', 'full'),
}


def font_codes(build):
    import re
    return {int(m, 16) for m in re.findall(r'\{0x([0-9A-Fa-f]+)u,', (build / 'font24.inc').read_text())}


def check_glyphs(s, codes, label):
    missing = {c[0] for row in s.rows for c in row if c[3] != 2 and c[0] and c[0] not in codes}
    require(not missing, f'{label}: characters not in the font: {[hex(c) for c in sorted(missing)]}')


def cyan(s, x, y):
    return s.gpx(x, y) == CYAN_RGB


def check_screen(s, kind, label):
    cols, rows = s.cols, s.nrows
    boxes = s.boxes()
    if kind == 'filer':
        check_frame(s, label)
    elif kind in ('full', 'grid', 'vnum'):
        check_screen_frame(s, label)
        if kind == 'grid':
            check_grid_joins(s, label)
        if kind == 'vnum':
            require(all(cyan(s, x, 40) and cyan(s, x, 41) for x in range(5, cols * 12 - 5)), f'{label}: the numbers\' line across the frame')
    elif kind == 'box':
        floating = [b for b in boxes if b[5] == 3]
        require(floating, f'{label}: a floating box {boxes}')
        col, row, w, h = floating[-1][:4]
        for y in range((row + 1) * 24, (row + h - 1) * 24, 5):
            require(cyan(s, col * 12 + 5, y) and cyan(s, (col + w) * 12 - 7, y), f'{label}: the box frame at y {y}')


def check_screen_frame(s, label):
    """The screen's frame (cells.h frame_open): box 0 over rows 0..rows-3 and
    every column, without a shadow; its lines to the edges, broken only by
    the title and the status; the title (YELLOW) in the top line; the lines
    inside it joined to it at both ends (┬ ┴ ├ ┤)."""
    cols, rows = s.cols, s.nrows
    boxes = s.boxes()
    require(boxes and boxes[0][:4] == [0, 0, cols, rows - 2] and boxes[0][5] == 1, f'{label}: the frame over rows 0..{rows - 3}: {boxes}')
    top_from, top_to, bottom_from, bottom_to = s.data['gaps'][0]
    left, right, top, bottom = 5, cols * 12 - 7, 11, (rows - 3) * 24 + 11
    covered = len(boxes) > 1   # a floating box over it hides a part
    if not covered:
        for y in range(top + 7, bottom - 6):
            require(cyan(s, left, y) and cyan(s, left + 1, y), f'{label}: left line at y {y}')
            require(cyan(s, right, y) and cyan(s, right + 1, y), f'{label}: right line at y {y}')
        for x in range(left + 7, right - 6):
            if not top_from * 12 <= x < top_to * 12:
                require(cyan(s, x, top) and cyan(s, x, top + 1), f'{label}: top line at x {x}')
            if not bottom_from * 12 <= x < bottom_to * 12:
                require(cyan(s, x, bottom) and cyan(s, x, bottom + 1), f'{label}: bottom line at x {x}')
    # The title: in the top line's gap, yellow.
    require(top_to > top_from and any(s.cell(x, 0)[1] == YELLOW for x in range(top_from, top_to)), f'{label}: the title in the top line {s.text(0)!r}')
    # The message line and the band are outside the frame (no frame line there).
    require(not any(cyan(s, left, y) for y in range((rows - 2) * 24, (rows - 1) * 24)), f'{label}: the frame ends above the message line')
    # Its lines: a horizontal one from the left line to the right line, a
    # vertical one from the top line (row 0) to the bottom line (row rows-3).
    lines = [l for l in s.data.get('lines', []) if l[4] == 0]
    for at, a, b, vertical, box in lines:
        if covered:
            continue
        if vertical:
            # Each end on the frame's top or bottom line or on a line across.
            # (Or next to a heading written over a line across, in the customizer.)
            joins = {0, rows - 3} | {h[0] for h in lines if not h[3] and h[1] <= at <= h[2]}
            headings = {h[0] for h in lines if not h[3] and not (h[1] <= at <= h[2])}
            require((a in joins or a - 1 in headings) and (b in joins or b + 1 in headings),
                    f'{label}: the line down column {at} ({a}..{b}) ends on lines {sorted(joins)}')
            x = at * 12 + 5
            require(all(cyan(s, x, y) and cyan(s, x + 1, y) for y in range(a * 24 + 11, b * 24 + 13)), f'{label}: the line down column {at} is whole')
        else:
            y = at * 24 + 11
            if a == 0 and b == cols - 1:
                require(all(cyan(s, x, y) and cyan(s, x, y + 1) for x in range(left, right + 2)), f'{label}: the line across row {at} is whole')


def check_grid_joins(s, label):
    """The table's one-dot lines run to the frame: a column's line from the
    top line to the bottom line, the last row's line from the left line."""
    cols, rows = s.cols, s.nrows
    top_from, top_to, bottom_from, bottom_to = s.data['gaps'][0]
    bottom = (rows - 3) * 24 + 11
    joined = [x for x in range(13, cols * 12 - 13)
              if not top_from * 12 <= x < top_to * 12 and not bottom_from * 12 <= x < bottom_to * 12
              and all(cyan(s, x, y) for y in list(range(13, 24)) + list(range(bottom - 11, bottom)))]
    require(len(joined) >= 4, f'{label}: the grid\'s column lines reach the top and bottom lines {joined}')
    last = (rows - 3) * 24 - 1
    require(all(cyan(s, x, last) for x in range(7, 24)), f'{label}: the last row\'s line from the left line')
    require(sum(1 for y in range(60, (rows - 3) * 24, 3) for x in range(0, cols * 12, 3) if cyan(s, x, y)) > 100, f'{label}: ruled cells')


def make_files(base):
    d = fixture(base)
    # A docx with a page break: its line goes across the frame.
    body = ''.join(f'<w:p><w:r><w:t>段落 {i + 1}</w:t></w:r></w:p>' for i in range(3))
    body += '<w:p><w:r><w:br w:type="page"/></w:r></w:p>' + ''.join(f'<w:p><w:r><w:t>次 {i + 1}</w:t></w:r></w:p>' for i in range(3))
    office.make_docx(d / 'break.docx', f'{office.XML_DECL}{office.DOCX_ROOT}<w:body>{body}</w:body></w:document>')
    with zipfile.ZipFile(d / 'arc.zip', 'w') as z:
        for i in range(30):
            z.writestr(f'dir/file{i:02d}.txt', 'x' * i)
    # fixture() makes deck.pptx only when check_slide.py has left its JPEG in
    # this build; in a fresh build there was no deck and the pptx screens
    # failed (REVISION1_W2_NOTES.md). Slide 1, the one shown, has no picture.
    if not (d / 'deck.pptx').exists():
        office.make_pptx(d / 'deck.pptx', b'')
    return d


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=ROOT / 'build')
    parser.add_argument('--png', action='store_true')
    parser.add_argument('--only', default='')
    args = parser.parse_args()
    build = args.build.resolve()
    (build / 'tmp').mkdir(parents=True, exist_ok=True)
    r = Runner(build, args.png)
    r.base = build / 'frames-check'
    r.env = dict(r.env, HOME=str(r.base / 'home'), FDTP_RC_USER=str(r.base / 'none.fdtprc'))
    shutil.rmtree(r.base / 'snaps', ignore_errors=True)
    d = make_files(r.base)
    codes = font_codes(build)
    for name, (keys, kind) in SCREENS.items():
        if args.only and args.only not in name:
            continue
        for size in SIZES:
            snap = r.run(f'{name}-{size}', f'{keys} SNAP', d, ['--size', size], expect=1)[0]
            check_screen(snap, kind, f'{name} {size}')
            if not name.startswith('hex'):   # the bytes of data.bin read as Shift_JIS are not ours
                check_glyphs(snap, codes, f'{name} {size}')
    print(f'frame checks passed: {r.count} snapshots, {check_wide.checks} checks')


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
