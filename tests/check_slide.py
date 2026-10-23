#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Stage 6 acceptance tests (pptx display, docs/FDTP_VIEWERS.md).

Builds tests/slide_harness.c with the sources of mk/slide.mk and their
supports, makes the fixtures with tools/make_test_office.py (the JPEG with
the harness, through libjpeg), opens them, and checks the canvas: text
pixels, picture colours, rectangles, rules, the grey boxes and the row 0
status. F5 slideshow checks cover navigation, fullscreen restoration, ignored
edits, and exact full-canvas geometry including black margins after resize.
Damaged packages must be reported, never crash. Runs once plainly
and once under ASan/UBSan. Everything is written under BUILD.

    python3 -B tests/check_slide.py --build build [--no-sanitize]
"""
import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('office', ROOT / 'tools/make_test_office.py')
office = importlib.util.module_from_spec(spec)
spec.loader.exec_module(office)
SUPPORT = ['i18n', 'slideview', 'doc_input', 'doc_undo', 'zipfile', 'img_decode', 'img_modern', 'img_bmp', 'img_legacy',
           'app', 'widgets', 'keys', 'commands', 'canvas', 'cells', 'raster', 'font24', 'jis']
SANITIZE = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=undefined']
NAVY = (0, 0, 85)
checks = 0


def require(condition, message):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)


def mk_sources():
    text = (ROOT / 'mk/slide.mk').read_text()
    return re.search(r'^SLIDE_SOURCES := (.*)$', text, re.M).group(1).split()


def build(build_dir, sanitize):
    (build_dir / 'tmp').mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(build_dir / 'tmp'))
    for inc, tool in (('font24.inc', 'gen_font.py'), ('jis0208.inc', 'gen_jis.py')):
        if not (build_dir / inc).exists():
            subprocess.run([sys.executable, str(ROOT / 'tools' / tool), '--output', str(build_dir / inc)], check=True, env=env)
    # The harness compiles src/*.c directly, so keep the catalog in step with them.
    subprocess.run([sys.executable, str(ROOT / 'tools/gen_i18n.py'), '--output', str(build_dir / 'i18n_en.inc')], check=True, env=env)
    exe = build_dir / ('slide-test-asan' if sanitize else 'slide-test')
    sources = [str(ROOT / 'src' / (n + '.c')) for n in mk_sources() + SUPPORT] + [str(ROOT / 'tests/slide_harness.c'), str(ROOT / 'tests/ime_fake.c')]
    flags = ['-O1', '-g', '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Werror'] + (SANITIZE if sanitize else [])
    if sys.platform == 'darwin':
        pkgs = ('jpeg-turbo', 'libpng', 'giflib', 'webp')
        cflags = [f'-I/opt/homebrew/opt/{p}/include' for p in pkgs]
        libs = [f'-L/opt/homebrew/opt/{p}/lib' for p in pkgs] + ['-ljpeg', '-lpng', '-lgif', '-lwebp', '-lz', '-lexpat', '-lm']
    else:
        pc = os.environ.get('PKG_CONFIG', 'pkg-config')
        mods = ['libjpeg', 'libpng', 'libwebp', 'zlib', 'expat']
        cflags = subprocess.run([pc, '--cflags'] + mods, capture_output=True, text=True, check=True).stdout.split()
        libs = subprocess.run([pc, '--libs'] + mods, capture_output=True, text=True, check=True).stdout.split() + ['-lgif', '-lm']
    subprocess.run([os.environ.get('CC', 'cc')] + flags + cflags + ['-I' + str(ROOT / 'src'), '-I' + str(build_dir)] + sources
                   + ['-o', str(exe)] + libs, check=True, env=env)
    return exe


def ppm(path, expected_size=(1024, 600)):
    data = Path(path).read_bytes()
    magic, wh, maxv, pixels = data.split(b'\n', 3)
    require(magic == b'P6' and maxv == b'255', f'{path}: not P6')
    w, h = map(int, wh.split())
    require((expected_size is None or (w, h) == expected_size) and len(pixels) == w * h * 3, f'{path}: size')
    return pixels


class Harness:
    def __init__(self, exe, env):
        self.exe, self.env = exe, env

    def open(self, deck, script, prefix, expected=(0,), env=None):
        r = subprocess.run([str(self.exe), 'open', str(deck), str(script), str(prefix)], capture_output=True,
                           env=dict(self.env, **(env or {})), timeout=120)
        out, err = r.stdout.decode('utf-8', 'replace'), r.stderr.decode('utf-8', 'replace')
        require(r.returncode in expected, f'{deck}: exit {r.returncode}, expected {expected}\n{out}\n{err}')
        require('runtime error' not in err and 'AddressSanitizer' not in err and 'LeakSanitizer' not in err, f'{deck}: {err}')
        snaps = []
        for line in out.splitlines():
            if line.startswith('SNAP '):
                parts = line.split(' ', 3)
                serial, depth = int(parts[1]), int(parts[2])
                notice = parts[3] if len(parts) > 3 else ''
                snap = {'depth': depth, 'notice': notice, 'image': None, 'cells': None}
                if depth:
                    snap['image'] = ppm(f'{prefix}-{serial:03d}.ppm', expected_size=None)
                    snap['cells'] = json.loads(Path(f'{prefix}-{serial:03d}.cells').read_text())
                snaps.append(snap)
        final = re.search(r'^FULLSCREEN ([01])$', out, re.M)
        self.fullscreen_after_destroy = int(final.group(1)) if final else None
        notice = re.search(r'^NOTICE (.*)$', out, re.M)
        return r.returncode, snaps, notice.group(1) if notice else ''


def row_text(snap, y):
    return ''.join(chr(c[0]) for c in snap['cells']['rows'][y] if c[3] != 2).rstrip()


# Revision 1 item 29: the slide in the body of the screen's frame, canvas
# x 44..980, y 24..528 at 80 x 25 (raster.h VIEW_LEFT, VIEW_W, VIEW_H).
VIEW_LEFT, VIEW_TOP, VIEW_W, VIEW_H = 44, 24, 936, 504


def status(snap):
    """The bottom line of the frame (row 22): スライド N/M."""
    return row_text(snap, 22).strip()


class Page:
    """Slide coordinates (EMU) to canvas pixels, as slideview.c places them."""

    def __init__(self, image, cx=office.PPTX_CX, cy=office.PPTX_CY,
                 viewport=(VIEW_LEFT, VIEW_TOP, VIEW_W, VIEW_H)):
        self.image = image
        left, top, width, height = viewport
        self.scale = min(width / cx, height / cy)
        self.w, self.h = int(cx * self.scale + .5), int(cy * self.scale + .5)
        self.left, self.top = left + (width - self.w) // 2, top + (height - self.h) // 2

    def px(self, x):
        return self.left + x * self.scale

    def py(self, y):
        return self.top + y * self.scale

    def at(self, x, y):
        """Pixel at canvas coordinates."""
        i = (int(y) * 1024 + int(x)) * 3
        return tuple(self.image[i:i + 3])

    def emu(self, x, y):
        return self.at(self.px(x), self.py(y))

    def box(self, x, y, w, h):
        """Canvas rectangle of an EMU box, as integers."""
        return (int(self.px(x) + .5), int(self.py(y) + .5), int(self.px(x + w) + .5), int(self.py(y + h) + .5))

    def ink(self, rect, dark=True):
        """Bounding box (x0, y0, x1, y1) of text pixels in rect, or None.
        dark: black text; otherwise white text."""
        x0, y0, x1, y1 = rect
        found = None
        for y in range(max(0, y0), min(600, y1)):
            row = y * 1024 * 3
            for x in range(max(0, x0), min(1024, x1)):
                p = self.image[row + x * 3:row + x * 3 + 3]
                hit = max(p) < 90 if dark else min(p) > 235
                if hit:
                    found = (x, y, x, y) if not found else (min(found[0], x), min(found[1], y), max(found[2], x), max(found[3], y))
        return found

    def bands(self, rect, gap=4):
        """Lines of dark text pixels: [(y0, y1, xmin)]; blank runs shorter
        than gap rows (inside a glyph such as 二) do not split a line."""
        x0, y0, x1, y1 = rect
        rows = []
        for y in range(y0, y1):
            xs = [x for x in range(x0, x1) if max(self.at(x, y)) < 90]
            if xs:
                rows.append((y, min(xs)))
        out = []
        for y, xmin in rows:
            if out and y - out[-1][1] <= gap:
                out[-1] = (out[-1][0], y, min(out[-1][2], xmin))
            else:
                out.append((y, y, xmin))
        return out


def near(a, b, tolerance=0):
    return all(abs(p - q) <= tolerance for p, q in zip(a, b))


def hexrgb(s):
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


def check_pages(h, deck, out):
    _, snaps, _ = h.open(deck, ROOT / 'tests/slide_pages.keys', out / 'pages')
    require(len(snaps) == 6 and all(s['depth'] == 1 for s in snaps), [s['depth'] for s in snaps])
    for i, s in enumerate(snaps):
        require('deck.pptx' in row_text(s, 0) and status(s) == f'スライド {i + 1}/6', (row_text(s, 0), status(s)))
        require(s['notice'] == '', (i, s['notice']))
        band = row_text(s, 24)
        require('SPC次' in band and 'BS前' in band and 'g番号' in band and 'q戻る' in band, band)
        require(s['cells']['rows'][24][2][2] == 3 and s['cells']['rows'][24][5][2] == 1, 'key on a yellow ground, its name on cyan')
    G, B = office.PPTX_GEOM, office.PPTX_BOX
    pages = [Page(s['image']) for s in snaps]
    p = pages[0]
    # 16:9: the height of the frame's body (504), in the middle across.
    require((p.w, p.h, p.left, p.top) == (896, 504, 64, 24), (p.w, p.h, p.left, p.top))
    for page in pages:
        require(page.at(5, 300) == NAVY and page.at(1020, 300) == NAVY, 'navy margins beside the slide')

    # 1: own background, title and subtitle placed by the layout (no xfrm on the slide).
    require(p.at(p.left + 3, p.top + 3) == hexrgb('FFF2CC'), f'slide background {p.at(p.left + 3, p.top + 3)}')
    title = p.box(*B['layout1_title'])
    ink = p.ink(title)
    require(ink is not None, 'title text inside the layout title box')
    require(38 <= ink[3] - ink[1] + 1 <= 48, f'title 60 pt drawn three times: {ink}')
    require(ink[1] > (title[1] + title[3]) / 2, f'title anchored at the bottom: {ink} in {title}')
    require(abs((ink[0] + ink[2]) / 2 - (title[0] + title[2]) / 2) < 12, f'title centred: {ink} in {title}')
    mx, my, mw, mh = B['master_title']
    require(p.ink(p.box(mx, my, mw, B['layout1_title'][1] - my - 20000)) is None, 'nothing at the master title position')
    sub = p.ink(p.box(*B['layout1_sub']))
    require(sub is not None and 16 <= sub[3] - sub[1] + 1 <= 24, f'subtitle 24 pt at 1.5 times: {sub}')
    require(p.ink(p.box(*B['layout1_sub']))[0] > p.px(B['layout1_sub'][0]) + 50, 'subtitle centred, no bullet')

    # 2: four sizes of ■, a red rectangle with a blue line, a theme-coloured shape.
    p = pages[1]
    heights = []
    for size, (x, y) in G['sizes']:
        box = p.ink(p.box(x, y, *G['size_box']))
        require(box is not None, f'text at {size}')
        require(0 <= box[0] - p.px(x) <= 8 and 0 <= box[1] - p.py(y) <= 16, f'text at the box corner {size}: {box}')
        heights.append(box[3] - box[1] + 1)
    base = heights[0]
    require(10 <= base <= 16, f'18 pt at one time: {heights}')
    for got, f in zip(heights[1:], (1.5, 2, 3)):
        require(abs(got - base * f) <= 2, f'factor {f}: {heights}')
    x, y, w, hh = G['rect']
    require(p.emu(x + w / 2, y + hh / 2) == (255, 0, 0), 'red fill')
    left, top = p.box(x, y, w, hh)[:2]
    require(p.at(left + 1, top + 20) == (0, 0, 255) and p.at(left + 5, top + 20) == (255, 0, 0), 'blue line 3 px inside the box')
    x, y, w, hh = G['style_rect']
    require(p.emu(x + w * .1, y + hh * .1) == hexrgb(office.PPTX_THEME['accent1']), 'fill from p:style and the theme')
    white = p.ink(p.box(x, y, w, hh), dark=False)
    require(white is not None and white[1] > p.py(y + hh * .3), f'white text from fontRef, centred: {white}')
    require(p.ink(p.box(600000, 300000, 10000000, 1000000)) is not None, 'title of slide 2')

    # 3: title from the master (layout without xfrm), body bullets by level.
    p = pages[2]
    t = p.ink(p.box(*B['master_title']))
    require(t is not None, 'title at the master position')
    mt = p.box(*B['master_title'])
    require(abs((t[1] + t[3]) / 2 - (mt[1] + mt[3]) / 2) < 6, f'master title anchor ctr: {t} {mt}')
    body = p.box(*B['layout2_body'])
    bands = p.bands(body)
    require(len(bands) == 2, f'two paragraphs: {bands}')
    (a0, a1, ax), (b0, b1, bx) = bands
    left = body[0] + 91440 * p.scale
    require(0 <= ax - left <= 16, f'level 1 bullet "・" at the left inset: {ax} vs {left}')
    require(abs((bx - ax) - (685800 - 228600) * p.scale) <= 5, f'level 2 indented by marL: {ax} {bx}')
    # The glyphs are 1, 1.5, 2 or 3 times the 16-dot font, the nearest to the
    # size (slide_render.c factor_for): 28 pt is drawn larger than 24 pt only
    # when they round to different sizes at this scale (both 24 dots in the
    # frame's body since revision 1 item 29).
    def dots(pt):
        return min((16, 24, 32, 48), key=lambda f: abs(f - pt * 12700 * p.scale))
    if dots(28) > dots(24):
        require(a1 - a0 > b1 - b0, f'28 pt over 24 pt: {bands}')
    else:
        require(a1 - a0 == b1 - b0, f'28 pt and 24 pt the same size here: {bands}')
    require(p.ink((int(ax), a0, int(ax + 6), a1 + 1)) is not None, 'the bullet')
    bw = dots(28)   # the full-width bullet at the paragraph's size, then the text
    require(p.ink((int(ax + 8), a0, int(left + bw - 1), a1 + 1)) is None, 'a gap after the bullet')
    require(p.ink((int(left + bw), a0, int(left + 2 * bw), a1 + 1)) is not None, 'the text after the bullet')

    # 4: pictures.
    p = pages[3]

    def frac(box, fx, fy):
        x, y, w, hh = box
        return p.emu(x + w * fx, y + hh * fy)
    require(near(frac(G['pic_full'], .25, .5), (255, 0, 0)) and near(frac(G['pic_full'], .75, .5), (0, 255, 0)), 'PNG')
    require(near(frac(G['pic_crop'], .1, .5), (0, 255, 0)) and near(frac(G['pic_crop'], .9, .5), (0, 255, 0)), 'cropped PNG')
    require(near(frac(G['pic_jpeg'], .5, .2), (0, 0, 255), 40) and near(frac(G['pic_jpeg'], .5, .8), (255, 255, 0), 40), 'JPEG')
    require(near(frac(G['pic_flip'], .25, .5), (0, 255, 0)) and near(frac(G['pic_flip'], .75, .5), (255, 0, 0)), 'flipped PNG')
    require(p.emu(300000, 300000) == (255, 255, 255), 'white page')

    # 5: group transform and a connector.
    p = pages[4]
    ox, oy, ex, ey, cx, cy = G['group']
    gx, gy, gw, gh = G['group_child']
    sx, sy = ex / cx, ey / cy
    centre = (ox + (gx + gw / 2) * sx, oy + (gy + gh / 2) * sy)
    require(p.emu(*centre) == hexrgb('7030A0'), 'group child moved and scaled')
    require(p.emu(gx + gw * .9, gy + gh * .9) == (255, 255, 255), 'nothing at the untransformed place')
    x, y, w, _ = G['connector']
    require(any(p.at(p.px(x + w / 2), p.py(y) + d) == hexrgb('FF6600') for d in (-1, 0, 1)), 'connector line')

    # 6: background from the layout, table, chart box, markup compatibility.
    p = pages[5]
    require(p.at(p.left + 3, p.top + 3) == hexrgb('DDEBF7'), 'layout background')
    tx, ty, tw, th = G['table']
    require(p.emu(tx + 2000000 * .9, ty + 800000 * .85) == hexrgb('FFC000'), 'header cell fill')
    col = p.px(tx + 2000000)
    require(any(p.at(col + d, p.py(ty + 1200000)) == (64, 64, 64) for d in (-1, 0, 1)), 'column rule')
    require(any(p.at(p.px(tx + 3000000), p.py(ty + 800000) + d) == (64, 64, 64) for d in (-1, 0, 1)), 'row rule')
    require(p.ink(p.box(tx + 2000000, ty + 800000, 2000000, 800000)) is not None, 'text in cell B2')
    require(p.ink(p.box(tx + 4000000 + 30000, ty + 800000 + 30000, 1900000, 700000)) is None, 'empty cell C2')
    x, y, w, hh = G['chart']
    require(p.emu(x + w * .1, y + hh * .1) == (200, 200, 200), 'grey box for the chart')
    label = p.ink(p.box(x, y, w, hh))
    require(label is not None and abs((label[0] + label[2]) / 2 - p.px(x + w / 2)) < 4, f'[グラフ] centred: {label}')
    x, y, w, hh = G['choice']
    require(p.emu(x + w / 2, y + hh / 2) == hexrgb('DDEBF7'), 'mc:Choice left out')
    x, y, w, hh = G['fallback']
    require(p.emu(x + w / 2, y + hh / 2) == hexrgb('00B0F0'), 'mc:Fallback drawn')


def check_keys(h, deck, out):
    _, snaps, _ = h.open(deck, ROOT / 'tests/slide_navigate.keys', out / 'navigate')
    where = [status(s).split()[-1] if s['depth'] else None for s in snaps]
    require(where == ['1/6', '6/6', '6/6', '5/6', '1/6', '4/6', '4/6', '4/6', None], where)
    require(snaps[6]['notice'] == 'スライド番号が違います', snaps[6]['notice'])
    require(snaps[8]['depth'] == 0, 'q goes back')
    require(snaps[5]['image'] == ppm(out / 'pages-004.ppm'), 'g 4 draws the same slide 4 as paging to it')


def check_slideshow(h, deck, out, jpeg):
    """F5 presents at canvas size, with navigation and no editing side effects."""
    original = deck.read_bytes()
    _, snaps, _ = h.open(deck, ROOT / 'tests/slide_slideshow.keys', out / 'slideshow')
    require(len(snaps) == 24, f'slideshow snapshots: {len(snaps)}')
    require(all(s['depth'] == 1 for s in snaps[:-1]) and snaps[-1]['depth'] == 0,
            'Esc/F5 leave the slideshow; q then leaves the viewer')
    require(snaps[0]['image'] == ppm(out / 'pages-001.ppm'), 'ordinary view before F5')
    # These are full-canvas snapshots; equal indices must render identically.
    indices = [1, 2, 1, 2, 1, 2, 1, 2, 3, 2, 6, 6, 6, 1, 1, 1, 1, 1, 1]
    reference = {}
    for index, snap in zip(indices, snaps[1:20]):
        reference.setdefault(index, snap['image'])
        require(snap['image'] == reference[index], f'presentation navigation to slide {index}')
        require(snap['cells']['fullscreen'] == 1, 'F5 requests platform fullscreen')
        require(snap['notice'] == '', 'editing/saving/goto shortcuts ignored while presenting')
    require(len(set(reference.values())) == 4, 'all visited slide images are distinct')
    p = Page(reference[2], viewport=(0, 0, 1024, 600))
    x, y, w, hh = office.PPTX_GEOM['rect']
    require(p.emu(x + w / 2, y + hh / 2) == (255, 0, 0), 'navigation reaches slide 2 red rectangle')
    p = Page(reference[6], viewport=(0, 0, 1024, 600))
    require(p.at(p.left + 3, p.top + 3) == hexrgb('DDEBF7'), 'End reaches slide 6 background')
    require(status(snaps[20]) == 'スライド 1/6' and snaps[20]['cells']['fullscreen'] == 0,
            'F5 returns to the current slide and restores windowed state')
    require(snaps[20]['image'] == snaps[0]['image'], 'F5 restores ordinary frame exactly')
    require(snaps[21]['image'] == reference[2] and snaps[21]['cells']['fullscreen'] == 1,
            'F5 starts on the current slide')
    require(status(snaps[22]) == 'スライド 2/6' and snaps[22]['cells']['fullscreen'] == 0,
            'Esc returns to the current slide and restores windowed state')
    require(deck.read_bytes() == original and not Path(str(deck) + '.bak').exists(),
            'presentation keys leave the package unchanged without creating a backup')

    # Solid slides make every page pixel and every black margin checkable,
    # including frame/status/key-band pixels which must never cover the page.
    for name, cx, cy in [('wide', 16000000, 9000000), ('standard', 12000000, 9000000),
                         ('portrait', 9000000, 16000000)]:
        parts = office.pptx_parts(jpeg, slides=[office.pptx_slide([], bg='73B5D9')],
                                  layouts_of=[2], extra_slide_rels={}, media=[])
        parts = [(n, re.sub(r'<p:sldSz[^>]*/>', f'<p:sldSz cx="{cx}" cy="{cy}"/>', d)
                  if n == 'ppt/presentation.xml' else d) for n, d in parts]
        solid = office.package(out / f'present-{name}.pptx', parts)
        script = out / f'present-{name}.keys'
        script.write_text('F5 SNAP DISPLAY:1920x1080 SNAP ESC SNAP F5 SNAP\n')
        _, geometry, _ = h.open(solid, script, out / f'present-{name}',
                                 env={'FDTP_TEST_FULLSCREEN': '1'})
        require(geometry[2]['cells']['fullscreen_sets'] == 1, 'exit does not request a fullscreen change when already fullscreen on entry')
        require(h.fullscreen_after_destroy == 1, 'destroy keeps pre-existing fullscreen')
        for snap, (width, height) in zip(geometry, [(1024, 600), (1920, 1080), (1920, 1080), (1920, 1080)]):
            require(snap['cells']['fullscreen'] == 1, 'pre-existing fullscreen is retained after exit')
            if snap is geometry[2]:
                continue
            scale = min(width / cx, height / cy)
            w, hh = int(cx * scale + .5), int(cy * scale + .5)
            left, top = (width - w) // 2, (height - hh) // 2
            black, colour = bytes((0, 0, 0)), bytes((0x73, 0xB5, 0xD9))
            expected = ((black * width) * top +
                        (black * left + colour * w + black * (width - left - w)) * hh +
                        (black * width) * (height - top - hh))
            require(snap['image'] == expected,
                    f'{name} {width}x{height}: fitted page, aspect ratio, centred black margins, no frame')

    # Exercise the environment interface used by scripted FDTP acceptance runs.
    prefix = out / 'present-env'
    script = out / 'present-env.keys'
    script.write_text('INVALID_TOKEN\n')  # environment script must take precedence
    _, env_snaps, _ = h.open(deck, script, prefix, env={
        'FDTP_TEST_KEYS': 'F5 SNAP ESC SNAP q SNAP', 'FDTP_SNAPSHOT_PREFIX': str(prefix)})
    require(len(env_snaps) == 3 and env_snaps[0]['image'] == reference[1] and
            status(env_snaps[1]) == 'スライド 1/6' and env_snaps[2]['depth'] == 0,
            'FDTP_TEST_KEYS and FDTP_SNAPSHOT_PREFIX drive presentation acceptance')
    _, active, _ = h.open(deck, script, out / 'present-destroy', env={
        'FDTP_TEST_KEYS': 'F5 SNAP', 'FDTP_SNAPSHOT_PREFIX': str(out / 'present-destroy')})
    require(active[0]['cells']['fullscreen'] == 1 and h.fullscreen_after_destroy == 0,
            'destroying an active slideshow restores a formerly windowed platform')


def check_broken(h, out, jpeg):
    files = office.make_broken_pptx(out / 'broken', jpeg)
    for name, path in files.items():
        if name == 'odd-slides':
            continue
        rc, snaps, notice = h.open(path, ROOT / 'tests/slide_broken.keys', out / f'broken-{name}', expected=(0, 3))
        if rc == 3:
            require(notice and 'メモリ' not in notice, f'{name}: a reason is reported: {notice!r}')
        else:
            require(snaps and snaps[-1]['depth'] == 0, f'{name}: back to the list')
    expect = {'not-zip': 'pptx を開けません', 'no-presentation': 'presentation.xml がありません',
              'no-slides': 'スライドがありません', 'bad-presentation': 'presentation.xml が壊れています'}
    for name, text in expect.items():
        rc, _, notice = h.open(files[name], ROOT / 'tests/slide_broken.keys', out / f'broken-{name}', expected=(3,))
        require(text in notice, f'{name}: {notice!r}')
    _, snaps, _ = h.open(files['odd-slides'], ROOT / 'tests/slide_broken.keys', out / 'odd')
    require([s['depth'] for s in snaps] == [1, 1, 1, 1, 1, 1, 0], [s['depth'] for s in snaps])
    require(snaps[1]['notice'] == 'スライド 2 を読めません', snaps[1]['notice'])
    p = Page(snaps[2]['image'])  # the error box closed
    require(p.emu(300000, 300000) == (255, 255, 255) and p.ink((p.left, p.top, p.left + p.w, 528)) is None, "a white page (inside the frame)")
    p = Page(snaps[4]['image'])
    require(snaps[4]['notice'] == '表示できない画像があります', snaps[4]['notice'])
    require(p.emu(2000000, 2000000) == (200, 200, 200) and p.emu(5500000, 2000000) == (200, 200, 200), 'grey boxes for the pictures')
    require(p.emu(500000, 500000) == (0, 255, 0), 'shape inside 60 nested groups')
    require(p.emu(50, 50) != (255, 0, 0), 'negative size not drawn')
    require(status(snaps[5]).endswith('1/4'), status(snaps[5]))


# ---- revision 1 item 41: editing the text ----
MARK = (255, 0, 255)
RPR_BOLD = '<a:rPr lang="ja-JP" altLang="en-US" sz="2400" b="1" dirty="0"><a:solidFill><a:srgbClr val="FF0000"/></a:solidFill></a:rPr>'
EPR_BLUE = ('<a:endParaRPr lang="ja-JP" altLang="en-US" sz="2000" dirty="0"><a:solidFill><a:srgbClr val="0000FF"/></a:solidFill>'
            '</a:endParaRPr>')
BR_RUN = '<a:rPr lang="ja-JP" sz="2000"/>'
EDIT_PARAS = {   # name: (original, after the edits)
    'title': ('<a:p><a:r><a:rPr lang="ja-JP" altLang="en-US" dirty="0"></a:rPr><a:t>日本語のタイトル</a:t></a:r></a:p>',
              '<a:p><a:r><a:rPr lang="ja-JP" altLang="en-US" dirty="0"></a:rPr><a:t>新しい題</a:t></a:r></a:p>'),
    'mixed': (f'<a:p><a:r>{RPR_BOLD}<a:t>太字</a:t></a:r><a:r><a:rPr lang="ja-JP" altLang="en-US" sz="2400" dirty="0"/>'
              '<a:t>と普通 &amp; &lt;記号&gt;</a:t></a:r></a:p>',
              f'<a:p><a:r>{RPR_BOLD}<a:t>太字と普通 &amp; &lt;記号&gt;!</a:t></a:r></a:p>'),
    'br': (f'<a:p><a:pPr algn="ctr"/><a:r>{BR_RUN}<a:t>一行目</a:t></a:r><a:br>{BR_RUN}</a:br><a:r>{BR_RUN}<a:t>二行目</a:t></a:r>'
           f'<a:endParaRPr lang="ja-JP" sz="2000"/></a:p>',
           f'<a:p><a:pPr algn="ctr"/><a:r>{BR_RUN}<a:t>一行目</a:t></a:r><a:br>{BR_RUN}</a:br><a:r>{BR_RUN}<a:t>二行目三</a:t></a:r>'
           f'<a:endParaRPr lang="ja-JP" sz="2000"/></a:p>'),
    'empty': (f'<a:p><a:pPr marL="0"/>{EPR_BLUE}</a:p>',
              f'<a:p><a:pPr marL="0"/><a:r>{EPR_BLUE.replace("endParaRPr", "rPr")}<a:t>空に足した</a:t></a:r>{EPR_BLUE}</a:p>'),
    'field': ('<a:p><a:fld id="{B6F15528-21DE-4FAA-801E-634DDDAF4B2B}" type="slidenum"><a:rPr lang="ja-JP"/><a:t>1</a:t></a:fld>'
              '<a:endParaRPr lang="ja-JP"/></a:p>', None),
    'link': ('<a:p><a:r><a:rPr lang="ja-JP"><a:hlinkClick r:id="" action="ppaction://hlinkshowjump?jump=nextslide"/></a:rPr>'
             '<a:t>次へ</a:t></a:r></a:p>', None),
    'element': ('<a:p/>', '<a:p><a:r><a:t>要素</a:t></a:r></a:p>'),
}


def edit_deck(path, jpeg):
    """Three slides: text to edit (runs, a:br, empty paragraphs, a field, a
    link), a table with a merged cell and a picture, and mc:Fallback."""
    P = EDIT_PARAS
    s1 = office.pptx_slide([
        office.pptx_sp(2, 'Title 1', [P['title'][0]], None, '<p:ph type="ctrTitle"/>'),
        office.pptx_sp(3, 'TextBox 2', [P[k][0] for k in ('mixed', 'br', 'empty', 'field', 'link', 'element')],
                       office.pptx_xfrm(600000, 2400000, 10000000, 4000000), body_pr=' wrap="square"')])

    def tc(text, extra=''):
        return f'<a:tc{extra}><a:txBody><a:bodyPr/><a:lstStyle/>{office.pptx_para([office.pptx_run(text, 1800)])}</a:txBody><a:tcPr/></a:tc>'
    table = ('<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id="2" name="表 1"/><p:cNvGraphicFramePr><a:graphicFrameLocks noGrp="1"/>'
             '</p:cNvGraphicFramePr><p:nvPr/></p:nvGraphicFramePr><p:xfrm><a:off x="1000000" y="1500000"/><a:ext cx="6000000" cy="1600000"/></p:xfrm>'
             '<a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/table"><a:tbl><a:tblPr firstRow="1"/>'
             '<a:tblGrid><a:gridCol w="3000000"/><a:gridCol w="3000000"/></a:tblGrid>'
             f'<a:tr h="800000">{tc("見出し", " gridSpan=" + chr(34) + "2" + chr(34))}{tc("かくれた", " hMerge=" + chr(34) + "1" + chr(34))}</a:tr>'
             f'<a:tr h="800000">{tc("行2")}{tc("数値")}</a:tr></a:tbl></a:graphicData></a:graphic></p:graphicFrame>')
    s2 = office.pptx_slide([table, office.pptx_pic(4, 'rId2', (7600000, 1500000, 2400000, 2400000))])
    alt = ('<mc:AlternateContent xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006">'
           '<mc:Choice xmlns:fdtpx="urn:fdtp:test-extension" Requires="fdtpx">'
           + office.pptx_sp(4, 'Choice', [office.pptx_para('選ばれない')], office.pptx_xfrm(600000, 3000000, 4000000, 1000000))
           + '</mc:Choice><mc:Fallback>'
           + office.pptx_sp(5, 'Fallback', [office.pptx_para('代わりの文字')], office.pptx_xfrm(600000, 3000000, 4000000, 1000000))
           + '</mc:Fallback></mc:AlternateContent>')
    s3 = office.pptx_slide([office.pptx_sp(2, 'TextBox 1', [office.pptx_para('三枚目の文字')], office.pptx_xfrm(600000, 600000, 6000000, 1000000)), alt])
    parts = office.pptx_parts(jpeg, slides=[s1, s2, s3], layouts_of=[1, 2, 2],
                              extra_slide_rels={2: [('rId2', 'image', '../media/image1.png')]},
                              media=[('ppt/media/image1.png', office.pptx_png())])
    return office.package(path, parts, stored=('ppt/media/image1.png',))


def raw_entry(path, info):
    """Local header, data and descriptor of one zip entry, as stored."""
    import struct
    data = Path(path).read_bytes()
    start = info.header_offset
    name_len, extra_len = struct.unpack('<HH', data[start + 26:start + 30])
    return data[start:start + 30 + name_len + extra_len + info.compress_size]


def marks(snap):
    """Pixels of the paragraph's frame (magenta) and their bounding box."""
    img, xs, ys = snap['image'], [], []
    for y in range(24, 528):
        row = y * 1024 * 3
        for x in range(0, 1024):
            if img[row + x * 3:row + x * 3 + 3] == bytes(MARK):
                xs.append(x)
                ys.append(y)
    return (len(xs), (min(xs), min(ys), max(xs), max(ys)) if xs else None)


def check_paragraph_xml(data, label):
    """What PowerPoint requires of a paragraph (CT_TextParagraph): pPr first,
    then runs, line breaks and fields, endParaRPr last; a run is rPr? t."""
    import xml.etree.ElementTree as ET
    root = ET.fromstring(data)
    A = '{' + office.DML + '}'
    for p in root.iter(A + 'p'):
        names = [c.tag.replace(A, '') for c in p]
        order = {'pPr': 0, 'r': 1, 'br': 1, 'fld': 1, 'endParaRPr': 2}
        require(all(n in order for n in names) and [order[n] for n in names] == sorted(order[n] for n in names)
                and names.count('pPr') <= 1 and names.count('endParaRPr') <= 1, f'{label}: paragraph children {names}')
        for r in p.findall(A + 'r'):
            kids = [c.tag.replace(A, '') for c in r]
            require(kids in (['t'], ['rPr', 't']), f'{label}: run children {kids}')
        for br in p.findall(A + 'br'):
            require([c.tag.replace(A, '') for c in br] in ([], ['rPr']), f'{label}: a:br children')


def check_edit(h, out, jpeg):
    import shutil
    import zipfile
    d = out / 'edit'
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    original = edit_deck(d / 'original.pptx', jpeg)
    deck = d / 'deck.pptx'
    shutil.copy(original, deck)
    os.chmod(deck, 0o640)
    _, s, _ = h.open(deck, ROOT / 'tests/slide_edit.keys', d / 'e')
    require(len(s) == 22, f'{len(s)} snapshots')
    n = [x['notice'] for x in s]

    def dirty(snap):
        return row_text(snap, 0).endswith('変更あり')

    def line(snap):
        return row_text(snap, 23).strip()
    band = row_text(s[0], 24)
    require(all(k in band for k in ('SPC次', 'e直す', 'C-s保存', 'C-z取消', 'C-yやり直し', 'q戻る')), band)
    require(len(s[0]['cells']['rows'][24]) == 80 and marks(s[0])[0] == 0 and not dirty(s[0]), 'viewing: no frame on the slide')
    require(line(s[1]) == '段落 1/7  日本語のタイトル' and '↑↓段落' in row_text(s[1], 24) and 'Esc戻る' in row_text(s[1], 24), (line(s[1]), row_text(s[1], 24)))
    count, box = marks(s[1])
    title = Page(s[1]['image']).box(*office.PPTX_BOX['layout1_title'])
    require(count > 100 and box and title[0] - 4 <= box[0] and box[2] <= title[2] + 4 and title[1] - 4 <= box[1] and box[3] <= title[3] + 4,
            f'the chosen title framed inside the title box: {box} in {title}')
    require(row_text(s[2], 23).startswith('  段落: 日本語のタイトル'), row_text(s[2], 23))
    require(line(s[3]) == '段落 1/7  新しい題' and dirty(s[3]), (line(s[3]), row_text(s[3], 0)))
    require(line(s[4]) == '段落 2/7  太字と普通 & <記号>!', line(s[4]))
    require(row_text(s[5], 23) == '  段落: 一行目↓二行目', row_text(s[5], 23))
    require(line(s[6]) == '段落 3/7  一行目 二行目三', line(s[6]))
    require(line(s[7]) == '段落 4/7  空に足した', line(s[7]))
    require(n[8] == 'フィールドを含むため、この段落は直せません', n[8])
    require(n[9] == 'リンクを含むため、この段落は直せません', n[9])
    require(line(s[10]) == '段落 7/7  要素', line(s[10]))
    require(n[11] == '取り消しました（スライド 1 の 7 段落目）', n[11])
    require(n[12] == 'やり直しました（スライド 1 の 7 段落目）', n[12])
    require(status(s[13]) == 'スライド 2/3' and line(s[13]) == '段落 1/3  見出し', (status(s[13]), line(s[13])))
    cell = Page(s[13]['image']).box(1000000, 1500000, 6000000, 800000)
    count, box = marks(s[13])
    require(count > 50 and box and cell[0] - 4 <= box[0] and box[2] <= cell[2] + 4 and cell[1] - 4 <= box[1] and box[3] <= cell[3] + 4,
            f'the table cell framed: {box} in {cell}')
    require(line(s[14]) == '段落 2/3  行を直した', line(s[14]))
    require('e直す' in row_text(s[15], 24) and marks(s[15])[0] == 0, 'viewing again')
    require(n[16] == '保存しました（元のファイルは .bak に残しました）' and not dirty(s[16]), n[16])
    require(n[17] == '取り消しました（スライド 2 の 2 段落目）' and dirty(s[17]) and status(s[17]) == 'スライド 2/3', (n[17], status(s[17])))
    require(n[18] == '保存しました' and not dirty(s[18]), n[18])
    require(status(s[19]) == 'スライド 3/3' and line(s[19]) == '段落 1/2  三枚目の文字', line(s[19]))
    require(n[20] == '互換用の内容の中にあるため、この段落は直せません', n[20])
    require(s[21]['depth'] == 0, 'q leaves when nothing is unsaved')
    # The file: slide 1's edited paragraphs only; slide 2 as it was; every other entry as stored.
    require(Path(str(deck) + '.bak').read_bytes() == original.read_bytes(), '.bak is the original')
    require((deck.stat().st_mode & 0o777) == 0o640 and not [p for p in d.iterdir() if p.name.startswith('.deck.pptx')], 'mode, no temporary file')
    with zipfile.ZipFile(deck) as z, zipfile.ZipFile(original) as o:
        require(z.testzip() is None and z.namelist() == o.namelist(), 'CRCs, the same entries in the same order')
        for name in o.namelist():
            if name in ('ppt/slides/slide1.xml', 'ppt/slides/slide2.xml'):
                require(z.getinfo(name).compress_type == o.getinfo(name).compress_type, f'{name}: method kept')
                continue
            require(raw_entry(deck, z.getinfo(name)) == raw_entry(original, o.getinfo(name)), f'{name} copied byte for byte')
        require(z.read('ppt/slides/slide2.xml') == o.read('ppt/slides/slide2.xml'), 'slide 2: the edit undone, as the original')
        new, old = z.read('ppt/slides/slide1.xml').decode(), o.read('ppt/slides/slide1.xml').decode()
    expected = old
    for name, (before, after) in EDIT_PARAS.items():
        require(expected.count(before) == 1, f'{name} once in the fixture')
        if after:
            expected = expected.replace(before, after)
    require(new == expected, f'slide 1: only the edited paragraphs changed\n{new}')
    check_paragraph_xml(new.encode(), 'slide 1')
    # Opened again: the saved text is read and drawn.
    (d / 'again.keys').write_text('e SNAP DOWN SNAP DOWN SNAP DOWN SNAP END SNAP q SNAP\n')
    _, a, _ = h.open(deck, d / 'again.keys', d / 'a')
    got = [line(x) for x in a[:5]]
    require(got == ['段落 1/7  新しい題', '段落 2/7  太字と普通 & <記号>!', '段落 3/7  一行目 二行目三', '段落 4/7  空に足した',
                    '段落 7/7  要素'], got)
    p = Page(a[0]['image'])
    require(p.ink(p.box(*office.PPTX_BOX['layout1_title'])) is not None, 'the saved title drawn')
    # UNDO_LEVELS=1: two edits, one undone, then nothing.
    shutil.copy(original, d / 'levels.pptx')
    (d / 'levels.keys').write_text('e RET C-u "A" RET DOWN RET C-u "B" RET C-z SNAP C-z SNAP C-y C-y SNAP UP DOWN SNAP ESC ESC SNAP y SNAP\n')
    _, v, _ = h.open(d / 'levels.pptx', d / 'levels.keys', d / 'l', env={'FDTP_UNDO_LEVELS': '1'})
    require(v[0]['notice'] == '取り消しました（スライド 1 の 2 段落目）' and v[1]['notice'] == '取り消す物はありません' and dirty(v[1]),
            [x['notice'] for x in v])
    require(v[2]['notice'] == 'やり直す物はありません' and line(v[3]) == '段落 2/7  B', (v[2]['notice'], line(v[3])))
    require('保存せずに戻りますか？' in ''.join(row_text(v[4], y) for y in range(1, 23)) and v[5]['depth'] == 0, 'asks before leaving unsaved')
    # A slide edited and undone is not written again: its entry stays as stored.
    shutil.copy(original, d / 'undone.pptx')
    (d / 'undone.keys').write_text('e RET C-u "X" RET C-z PGDN PGDN RET C-u "Y" RET C-s SNAP\n')
    _, u, _ = h.open(d / 'undone.pptx', d / 'undone.keys', d / 'n')
    require(u[0]['notice'].startswith('保存しました'), u[0]['notice'])
    with zipfile.ZipFile(d / 'undone.pptx') as z, zipfile.ZipFile(original) as o:
        for name in o.namelist():
            if name == 'ppt/slides/slide3.xml':
                require(b'<a:t>Y</a:t>' in z.read(name), 'slide 3 edited')
            else:
                require(raw_entry(d / 'undone.pptx', z.getinfo(name)) == raw_entry(original, o.getinfo(name)), f'{name} as stored')
    # The input method (tests/ime_fake.c): its candidates over the slide, above row 23.
    shutil.copy(original, d / 'ime.pptx')
    (d / 'ime.keys').write_text('e RET C-u M-SPC "nihon" SPC SNAP RET M-SPC RET SNAP ESC ESC SNAP y SNAP\n')
    _, m, _ = h.open(d / 'ime.pptx', d / 'ime.keys', d / 'i')
    rows = [row_text(m[0], y) for y in range(19, 23)]
    require(any('日本' in r for r in rows) and any('二本' in r for r in rows), rows)
    blue = sum(1 for y in range(20 * 24, 23 * 24) for x in range(0, 1024, 4)
               if m[0]['image'][(y * 1024 + x) * 3:(y * 1024 + x) * 3 + 3] == b'\x00\x00\xff')
    require(blue > 100, f'the candidates drawn over the slide ({blue} blue pixels)')
    require(line(m[1]) == '段落 1/7  日本' and dirty(m[1]), line(m[1]))
    # 85 x 25 (the Pomera): the bands and row 23 within the width.
    shutil.copy(original, d / 'wide.pptx')
    (d / 'wide.keys').write_text('SNAP e SNAP RET SNAP ESC ESC q SNAP\n')
    _, w, _ = h.open(d / 'wide.pptx', d / 'wide.keys', d / 'w', env={'FDTP_TEST_SIZE': '85x25'})
    require(all(len(x['cells']['rows'][24]) == 85 for x in w[:3]), 'drawn at 85 columns')
    require(row_text(w[0], 24).endswith('q戻る') and row_text(w[1], 24).endswith('Esc戻る'), (row_text(w[0], 24), row_text(w[1], 24)))
    require(line(w[1]) == '段落 1/7  日本語のタイトル' and marks(w[1])[0] > 100, 'choosing at 85 columns')
    require(row_text(w[2], 23).startswith('  段落: 日本語のタイトル') and w[3]['depth'] == 0, row_text(w[2], 23))


def run(exe, out, env, jpeg):
    h = Harness(exe, env)
    deck = office.make_pptx(out / 'deck.pptx', jpeg)
    check_pages(h, deck, out)
    check_keys(h, deck, out)
    check_slideshow(h, deck, out, jpeg)
    check_broken(h, out, jpeg)
    check_edit(h, out, jpeg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', default='build')
    ap.add_argument('--no-sanitize', action='store_true')
    args = ap.parse_args()
    build_dir = Path(args.build).resolve()
    tmp = build_dir / 'tmp'
    tmp.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(tmp))
    runs = [False] + ([] if args.no_sanitize else [True])
    for sanitize in runs:
        exe = build(build_dir, sanitize)
        out = build_dir / ('slide-check-asan' if sanitize else 'slide-check')
        out.mkdir(parents=True, exist_ok=True)
        jpeg_path = out / 'image2.jpeg'
        r = subprocess.run([str(exe), 'make-jpeg', str(jpeg_path)], env=env)
        require(r.returncode == 0, 'make-jpeg')
        run(exe, out, dict(env, ASAN_OPTIONS='detect_leaks=0' if sys.platform == 'darwin' else 'detect_leaks=1'), jpeg_path.read_bytes())
        leftovers = [p.name for p in tmp.glob('fdtp-slide-*')]
        require(not leftovers, f'temporary picture files left: {leftovers}')
        print(f'{"sanitized" if sanitize else "plain"}: ok')
    print(f'check_slide: {checks} checks passed')


if __name__ == '__main__':
    main()
