#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The dot pictures at the top right of the browser download page
(fdtp-localsend serve): a Pomera, open, with FD's navy screen, and a
Pomeranian. gen_web_art.py [--output FILE] [--preview FILE]

  --output FILE   the C header (default src/localsend_art.h, committed)
  --preview FILE  an HTML page with the pictures 4 times larger (to look at)

Each picture is a grid of letters, one per dot; the SVG has one path per
colour, made of the runs of each row, drawn with crisp edges so that the
dots stay square. Only the standard library is used.
"""
import argparse
from pathlib import Path

HERE = Path(__file__).resolve().parent

COLOURS = {
    'G': '#5d6470',   # the Pomera's body
    'g': '#8a929e',   # keys, hinge
    'N': '#000055',   # FD's navy
    'C': '#00ffff',   # FD's cyan
    'Y': '#ffff00',   # FD's yellow letters
    'W': '#ffffff',   # fur
    'w': '#dfe4ee',   # fur, the fluffy edge and shade
    's': '#b9c0d0',   # fur, the shadow side
    'E': '#f8b8c6',   # inside the ears, cheeks
    'K': '#1d1d24',   # eyes, nose
    'H': '#ffffff',   # the light in the eyes
    'M': '#3a2228',   # mouth
    'P': '#f47a96',   # tongue
    'p': '#d85d7c',   # tongue, the tip
    't': '#ffffff',   # the tail (marked while drawing)
}

POMERA = [
    '..GGGGGGGGGGGGGGGG..',
    '..GCCCCCCCCCCCCCCG..',
    '..GCNNNNNNNNNNNNCG..',
    '..GCNYYNYNNNNNNNCG..',
    '..GCNNNNNNNNNNNNCG..',
    '..GCNYYYNYYNNNNNCG..',
    '..GCNNNNNNNNNNNNCG..',
    '..GCNYNYYYNNNNNNCG..',
    '..GCNNNNNNNNNNNNCG..',
    '..GCCCCCCCCCCCCCCG..',
    '..GGGGGGGGGGGGGGGG..',
    '...gggggggggggggg...',
    '.GGGGGGGGGGGGGGGGGG.',
    'GGgGgGgGgGgGgGgGgGGG',
    'GgGgGgGgGgGgGgGgGgGG',
    'GGGGGggggggggGGGGGGG',
    '.GGGGGGGGGGGGGGGGGG.',
]

def pomeranian():
    """A white Pomeranian looking up, tongue out (the user's photo), drawn
    after the breed's look: one round ball of fluff with an apple head, big
    eyes, a short muzzle, small triangular ears standing apart, a ruff on
    the chest and the curled tail showing over the back. The outline is
    computed, then the face is put on."""
    w, h = 28, 24
    grid = [['.'] * w for _ in range(h)]

    def blob(cx, cy, rx, ry):
        return lambda x, y: ((x + .5 - cx) / rx) ** 2 + ((y + .5 - cy) / ry) ** 2 <= 1
    parts = [blob(23.0, 12.5, 3.6, 4.0),          # the tail, over the back
             blob(13.5, 11.2, 10.4, 8.6),         # the apple head
             blob(13.5, 18.2, 11.6, 5.4)]         # the body, all fluff
    for y in range(h):
        for x in range(w):
            if any(part(x, y) for part in parts):
                grid[y][x] = 'W'

    def put(x, y, rows):
        for dy, row in enumerate(rows):
            for dx, c in enumerate(row):
                if c != ' ':
                    grid[y + dy][x + dx] = c
    put(6, 0, [' w', ' ww', 'wEw', 'wEEw'])         # small triangular ears, apart
    put(18, 0, ['  w', ' ww', ' wEw', 'wEEw'])
    # Fluff: the edge light grey (darker on the lower right), and every third
    # edge dot grows a tuft outward.
    edges = []
    for y in range(h):
        for x in range(w):
            if grid[y][x] != 'W':
                continue
            out = [(dx, dy) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))
                   if not (0 <= x + dx < w and 0 <= y + dy < h) or grid[y + dy][x + dx] == '.']
            if out:
                edges.append((x, y, out))
    for x, y, out in edges:
        grid[y][x] = 's' if x + y > 32 else 'w'
        if (x + 2 * y) % 3 == 0 and y > 4:
            dx, dy = out[0]
            if 0 <= x + dx < w and 0 <= y + dy < h:
                grid[y + dy][x + dx] = 's' if x + y > 32 else 'w'
    # The ruff under the chin, and the tail's own shade.
    for x, y in ((9, 17), (12, 18), (15, 18), (18, 17), (10, 20), (14, 21), (17, 20)):
        grid[y][x] = 'w'                           # tufts of the ruff
    for y in range(9, 17):
        for x in range(22, 27):
            if grid[y][x] == 'W' and (x + y) % 2:
                grid[y][x] = 'w'
    put(6, 7, [' KK ', 'KHKK', 'KKKK', ' KK '])     # big round eyes, a light in each
    put(17, 7, [' KK ', 'KHKK', 'KKKK', ' KK '])
    put(4, 11, ['EE'])                             # cheeks
    put(22, 11, ['EE'])
    put(12, 11, ['KKK', ' K '])                    # a small nose, a short muzzle
    put(11, 12, ['M   M', ' MPM ', ' PPP ', '  p  '])  # smile, tongue out
    return [''.join(row) for row in grid]


POMERANIAN = pomeranian()

def svg(grid, label, scale):
    height, width = len(grid), max(len(row) for row in grid)
    paths = {}
    for y, row in enumerate(grid):
        x = 0
        while x < len(row):
            c = row[x]
            run = x
            while run < len(row) and row[run] == c:
                run += 1
            if c != '.':
                paths.setdefault(c, []).append(f'M{x} {y}h{run - x}v1h-{run - x}z')
            x = run
    body = ''.join(f'<path fill="{COLOURS[c]}" d="{"".join(d)}"/>' for c, d in paths.items())
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
            f'width="{width * scale:g}" height="{height * scale:g}" shape-rendering="crispEdges" '
            f'role="img" aria-label="{label}">{body}</svg>')


def c_string(name, text):
    return f'static const char {name}[] =\n    "{text.replace(chr(34), chr(92) + chr(34))}";\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', default=str(HERE.parent / 'src' / 'localsend_art.h'))
    ap.add_argument('--preview')
    args = ap.parse_args()
    pomera, dog = svg(POMERA, 'Pomera', 2.6), svg(POMERANIAN, 'Pomeranian', 2.2)
    Path(args.output).write_text(
        '/* Made by tools/gen_web_art.py (the dot grids are there): the pictures at\n'
        ' * the top right of the browser download page. Do not edit by hand. */\n'
        '#ifndef FDTP_LOCALSEND_ART_H\n#define FDTP_LOCALSEND_ART_H\n'
        + c_string('web_art_pomera', pomera) + c_string('web_art_pomeranian', dog) + '#endif\n')
    if args.preview:
        big = lambda s: s.replace(f'width="', 'data-w="', 1)
        Path(args.preview).write_text(
            '<!doctype html><body style="background:#000055;padding:24px">'
            f'<div style="display:flex;gap:24px;align-items:flex-end">{pomera}{dog}</div>'
            f'<div style="display:flex;gap:24px;align-items:flex-end;margin-top:24px;zoom:4">{pomera}{dog}</div></body>')


if __name__ == '__main__':
    main()
