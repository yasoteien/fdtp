#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Novel editor tests (docs/FDTP_NOVEL.md, stage E): check_novel.py --build DIR [--no-sanitize]

Builds the project into DIR (and, with AddressSanitizer/UBSan, into
DIR/novel-asan), links tests/novel_harness.c against the same objects (once
with the platform input method, once with tests/ime_fake.c), runs the
tests/novel_*.keys scripts on generated fixtures and checks the editor state
(JSON printed per SNAP), the canvas (PPM per SNAP) and the saved files.
Everything is written under DIR/novel-check; HOME, the state file and the
autosave directory point there too, so the real home is never touched.
"""
import argparse
import codecs
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / 'tests'
PAL = {0: (0, 0, 0x55), 1: (0, 255, 255), 2: (255, 255, 255), 3: (255, 255, 0),
       4: (255, 0, 255), 5: (0, 0, 0), 6: (0, 0, 255), 7: (255, 0, 0), 8: (0, 255, 0)}
BG, CYAN, WHITE, YELLOW, MAGENTA, RED = 0, 1, 2, 3, 4, 7
# Revision 1 item 29: the editor in the screen's frame (cells.h), the text in
# its body, canvas x 44 .. 980 and y 24 .. 528 at 80 x 25 (the grid from x 32).
FRAME_TOP_Y, FRAME_LEFT_X, FRAME_RIGHT_X, FRAME_BOTTOM_Y = 11, 37, 985, 539   # the frame's lines (two dots)
BODY_LEFT, BODY_RIGHT, BODY_BOTTOM = 44, 980, 528
H_BODY, H_RUBY, H_PITCH, H_ROWS = 36, 24, 36, 14   # horizontal: row 0 body/ruby y, row pitch, rows
LARGE_LINES = 6800                              # about 1.04 MB
V_BODY, V_PITCH, V_COLUMNS = 944, 36, 26        # vertical: column 0 body x, column pitch, columns
V_TOP, V_NUM_TOP, V_CHARS = 24, 44, 21         # vertical: text top without/with line numbers, characters
V_NUM_Y, V_NUM_RULE_Y, H_NUM_RULE_X = 26, 40, 97   # numbers' y; lines (two dots thick, CYAN)
H_NUM_BODY = 104                               # horizontal text x with line numbers


class Failure(Exception):
    pass


def require(condition, message):
    if not condition:
        raise Failure(message)


# ---- build -------------------------------------------------------------
def build(directory, sanitize):
    flags = ['-Werror'] + (['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitize else [])
    ldflags = ['-fsanitize=address,undefined'] if sanitize else []
    (directory / 'tmp').mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(directory / 'tmp'))
    make = ['make', '-s', '-C', str(ROOT), f'BUILD={directory}', 'CFLAGS=' + ' '.join(flags), 'LDFLAGS=' + ' '.join(ldflags)]
    subprocess.run(make + ['all'], check=True, env=env)
    info = subprocess.run(make + ['novel-link-info'], check=True, env=env, capture_output=True, text=True).stdout
    objects = libraries = None
    for line in info.splitlines():
        if line.startswith('OBJS '):
            objects = line[5:].split()
        elif line.startswith('LIBS '):
            libraries = shlex.split(line[5:])
    require(objects and libraries is not None, 'novel-link-info output')
    require(any(o.endswith('/noveledit.o') for o in objects), 'noveledit in the build')
    cc = shlex.split(os.environ.get('CC', 'cc'))
    base = cc + ['-std=c11', '-Wall', '-Wextra'] + flags + ['-I' + str(ROOT / 'src'), '-I' + str(directory)]
    harness = directory / 'novel-harness'
    fake = directory / 'novel-harness-fake'
    subprocess.run(base + [str(TESTS / 'novel_harness.c')] + objects + ['-o', str(harness)] + libraries, check=True, env=env)
    plain = [o for o in objects if not o.endswith(('/ime_none.o', '/ime_uim.o'))]
    subprocess.run(base + [str(TESTS / 'novel_harness.c'), str(TESTS / 'ime_fake.c')] + plain +
                   ['-o', str(fake)] + libraries, check=True, env=env)
    unit = directory / 'novel-unit'
    subprocess.run(base + [str(TESTS / 'novel_unit.c')] + objects + ['-o', str(unit)] + libraries, check=True, env=env)
    return harness, fake, unit


# ---- running -----------------------------------------------------------
class Runner:
    def __init__(self, work, harness, fake, label):
        self.work, self.harness, self.fake, self.label = work, harness, fake, label
        self.home = work / 'home'
        self.autosave = work / 'autosave'
        self.env = dict(os.environ, FDTP_LANGUAGE='ja', HOME=str(self.home), FDTP_NOVEL_AUTOSAVE=str(self.autosave),
                        TMPDIR=str(work / 'tmp'), ASAN_OPTIONS='halt_on_error=1:detect_leaks=0',
                        UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        self.env.pop('FDTP_NOVEL_STATE', None)
        self.runs = 0
        self.timings = {}

    def run(self, path, keys, name, fake=False, expect=0):
        out = self.work / 'snaps'
        out.mkdir(parents=True, exist_ok=True)
        prefix = out / name
        script = keys if isinstance(keys, Path) else TESTS / keys
        started = time.monotonic()
        r = subprocess.run([str(self.fake if fake else self.harness), str(path), str(script), str(prefix)],
                           capture_output=True, text=True, env=self.env, timeout=900)
        self.timings[name] = time.monotonic() - started
        self.runs += 1
        require('runtime error' not in r.stderr and 'AddressSanitizer' not in r.stderr,
                f'{self.label}/{name}: sanitizer\n{r.stderr[-3000:]}')
        require(r.returncode == expect, f'{self.label}/{name}: exit {r.returncode}\n{r.stderr[-2000:]}')
        snaps = [json.loads(line) for line in r.stdout.splitlines() if line.strip()]
        for s in snaps:
            if 'serial' in s and 'state' in s:
                s['ppm'] = Path(f'{prefix}-{s["serial"]:03d}.ppm')
        return snaps


class Image:
    def __init__(self, path):
        data = path.read_bytes()
        magic, size, maxval, pixels = data.split(b'\n', 3)
        require(magic == b'P6' and maxval == b'255', f'{path}: PPM')
        self.w, self.h = map(int, size.split())
        require((self.w, self.h) == (1024, 600) and len(pixels) == 1024 * 600 * 3, f'{path}: size')
        self.p = pixels

    def at(self, x, y):
        i = (y * self.w + x) * 3
        return tuple(self.p[i:i + 3])

    def box(self, x, y, w, h):
        for yy in range(max(0, y), min(self.h, y + h)):
            for xx in range(max(0, x), min(self.w, x + w)):
                yield xx, yy, self.at(xx, yy)

    def count(self, x, y, w, h, color=None):
        if color is None:
            return sum(1 for _, _, c in self.box(x, y, w, h) if c != PAL[BG])
        return sum(1 for _, _, c in self.box(x, y, w, h) if c == PAL[color])

    def ink_bbox(self, x, y, w, h, background):
        points = [(xx, yy) for xx, yy, c in self.box(x, y, w, h) if c != background]
        if not points:
            return None
        xs, ys = [p[0] for p in points], [p[1] for p in points]
        return min(xs), min(ys), max(xs), max(ys)


def image(snap):
    if '_image' not in snap:
        snap['_image'] = Image(snap['ppm'])
    return snap['_image']


def G(snap, code=None, kind=None, y=None, x=None):
    """Glyphs drawn: [code, x, y, w, h, color, kind]; kind 0 body, 1 ruby,
    2 reversed body, 3 emphasis dot, 4 candidate, 5 line number (code is
    the number)."""
    found = []
    for g in snap['state']['glyphs']:
        c, gx, gy, w, h, color, k = g
        if code is not None and c != (ord(code) if isinstance(code, str) else code):
            continue
        if kind is not None and k != kind:
            continue
        if y is not None and gy != y:
            continue
        if x is not None and gx != x:
            continue
        found.append({'code': c, 'x': gx, 'y': gy, 'w': w, 'h': h, 'color': color, 'kind': k})
    return found


def one(snap, code, **filters):
    found = G(snap, code, **filters)
    require(len(found) == 1, f'expected one {code!r} {filters}: {found}')
    return found[0]


def text(snap):
    return snap['state']['text']


def check_ink(snap, label):
    """Every glyph box has ink; colored bases show their color on the canvas."""
    im = image(snap)
    for g in G(snap):
        if g['code'] in (0x20, 0x3000, 0):
            continue
        require(im.count(g['x'], g['y'], g['w'], g['h']) >= 3, f'{label}: no ink for {g}')
        if g['kind'] in (0, 1, 3) and g['color'] in (MAGENTA, RED, CYAN):
            require(im.count(g['x'], g['y'], g['w'], g['h'], g['color']) >= 3, f'{label}: color of {g}')


def check_frame(snap, label):
    im = image(snap)
    require(im.count(32, 576, 960, 24, YELLOW) > 50, f'{label}: command band')
    require(im.count(32, 0, 960, 24, YELLOW) > 50, f'{label}: the name in the top line')
    # The frame: its top line (popups may cover part of it, the name is in a
    # gap), its left line and its bottom line, the status in the bottom line.
    require(im.count(32, FRAME_TOP_Y, 960, 2, CYAN) >= 960, f'{label}: the frame top line')
    require(im.count(FRAME_LEFT_X, 30, 2, 480, CYAN) >= 700, f'{label}: the frame left line')
    require(im.count(32, FRAME_BOTTOM_Y, 960, 2, CYAN) >= 960 and im.count(32, 528, 960, 24, CYAN) > 50, f'{label}: the frame bottom line and the status')
    st = snap['state']
    if st['caret'][0] and st['prompt'] == 0:
        x, y = st['caret'][1], st['caret'][2]
        # Row 23 (a notice) may cover the lowest text row.
        height = 24 if not snap.get('notice') else max(0, min(24, 552 - y))
        bar = im.count(x - 1, y, 3, height, YELLOW) if not st['vertical'] else im.count(x, y - 1, 24, 3, YELLOW)
        require(bar >= min(30, 3 * height - 6) or (st['vertical'] and snap.get('notice') and y > 549),
                f'{label}: caret bar at {x},{y}')


def notice(snap):
    return snap.get('notice', '')


# ---- fixtures ----------------------------------------------------------
RUBY = ('あ｜本気《マジ》い漢字《かんじ》う東京（とうきょう）え大阪(おおさか)お《《傍点》》か\n'
        'き｜《そのまま》く｜(かっこ)け｜（全角）こ\n'
        '改行を｜またぐ《\n'
        'るび》物\n'
        '｜一二三四五六七八九十百《かず》と｜親《あいうえおかきくけこさしすせそたちつてとな》さ\n'
        '長い｜例《ながいながいながいるびです》の\n'
        'ABCー12。ぁX\n')


def fixtures(directory):
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    f = {}
    def put(name, data):
        p = directory / name
        p.write_bytes(data if isinstance(data, bytes) else data.encode('utf-8'))
        f[name] = p
    put('ruby.txt', RUBY)
    put('vert.txt', RUBY)
    put('edit.txt', 'abc\ndef\n')
    put('sjis.txt', '漢字《かんじ》テスト\r\n二行目\r\n'.encode('cp932'))
    put('search.txt', '猫と犬。猫が好き。\n犬より猫。\n')
    put('auto.txt', '元の文\n')
    put('close.txt', 'c\n')
    put('ime.txt', 'ねこ\n')
    put('euc.txt', '漢字のテスト\n'.encode('euc_jp'))
    put('bom.txt', codecs.BOM_UTF8 + 'テスト\n'.encode())
    put('bad.txt', b'abc\x81 def\n')
    lines = [f'行{i:03d}' for i in range(200)]
    lines[5] = '長' * 100
    put('scroll.txt', '\n'.join(lines) + '\n')
    big = []
    for i in range(LARGE_LINES):
        big.append(f'第{i:05d}行。｜本気《マジ》で漢字《かんじ》を書く。東京（とうきょう）と《《傍点》》。あいうえおかきくけこ' +
                   ('目印' if i == 4000 else ''))
    put('large.txt', '\n'.join(big) + '\n')
    numbered = ['一行目', 'あ' * 50, '三'] + [f'行{i:03d}' for i in range(4, 14)] + ['長' * 100] + \
               [f'行{i:03d}' for i in range(15, 121)]
    put('numbers.txt', '\n'.join(numbered) + '\n')
    put('longline.txt', 'あいうえお' * 19990 + '漢字《かんじ》' * 10)
    f['new.txt'] = directory / 'new.txt'
    saveas = directory / 'saveas'
    saveas.mkdir()
    for name, data in (('src.txt', '漢字\r\n二行目\r\n'.encode('cp932')), ('a.txt', b'A\n'), ('exist.txt', b'E\n'),
                       ('b.txt', b'B\n'), ('c.txt', b'C\n')):
        (saveas / name).write_bytes(data)
    f['saveas'] = saveas
    return f


# ---- scenarios ---------------------------------------------------------
def scenario_ruby(r, f):
    s = r.run(f['ruby.txt'], 'novel_ruby.keys', 'ruby')
    require(len(s) == 7, f'ruby snaps {len(s)}')
    a = s[0]
    for label, snap in zip('1234567', s):
        check_ink(snap, f'ruby/{label}')
        check_frame(snap, f'ruby/{label}')
    # Line 0, collapsed: rubies above their bases, inside the group.
    for base, ruby in (('本気', 'マジ'), ('漢字', 'かんじ'), ('東京', 'とうきょう'), ('大阪', 'おおさか')):
        b0, b1 = one(a, base[0], y=H_BODY), one(a, base[1], y=H_BODY)
        near = [g for g in G(a, kind=1, y=H_RUBY) if chr(g['code']) in ruby and b0['x'] - 24 <= g['x'] <= b1['x'] + 36]
        xs = sorted(g['x'] for g in near)
        require(''.join(chr(g['code']) for g in sorted(near, key=lambda g: g['x'])) == ruby, f'ruby glyphs for {base}: {near}')
        require(all(g['y'] + 12 == b0['y'] for g in near), f'{ruby} above {base}')
        middle = (b0['x'] + b1['x'] + 24) / 2
        require(abs((xs[0] + xs[-1] + 12) / 2 - middle) <= 6, f'{ruby} centred over {base}: {xs} {middle}')
    require(one(a, '本', y=H_BODY)['color'] == WHITE and one(a, '漢', y=H_BODY)['color'] == WHITE, 'cross-site rubies white')
    require(one(a, '東', y=H_BODY)['color'] == MAGENTA and one(a, '阪', y=H_BODY)['color'] == MAGENTA, 'bracket rubies magenta')
    dots = G(a, kind=3, y=H_RUBY)
    bo, ten = one(a, '傍', y=H_BODY), one(a, '点', y=H_BODY)
    require(bo['color'] == MAGENTA and ten['color'] == MAGENTA, 'emphasis magenta')
    require(sorted(d['x'] for d in dots) == [bo['x'] + 6, ten['x'] + 6], f'emphasis dots {dots}')
    for code in (0xff5c, 0x300a, 0x300b, 0xff08, 0xff09, ord('('), ord(')')):
        require(not G(a, code, y=H_BODY), f'notation {code:x} hidden on line 0')
    # Line 1: escapes keep the symbol, hide the bar.
    y1 = H_BODY + H_PITCH
    require(G(a, '《', y=y1) and G(a, '》', y=y1) and G(a, '(', y=y1) and G(a, '（', y=y1), 'escaped symbols shown')
    require(not G(a, 0xff5c, y=y1) and not G(a, kind=1, y=y1 - 12), 'escape bars hidden, no ruby')
    # Lines 2-3: no ruby across a line end.
    require(G(a, '《', y=H_BODY + 2 * H_PITCH) and G(a, '》', y=H_BODY + 3 * H_PITCH), 'line-crossing notation literal')
    require(not G(a, kind=1, y=H_RUBY + 2 * H_PITCH) and not G(a, kind=1, y=H_RUBY + 3 * H_PITCH), 'no ruby across lines')
    # Line 4: over Naro's limits -> red.
    y4 = H_BODY + 4 * H_PITCH
    require(one(a, '一', y=y4)['color'] == RED and one(a, '百', y=y4)['color'] == RED, 'base of 11 red')
    require(one(a, '親', y=y4)['color'] == RED, 'kana ruby of 21 red')
    require(len(G(a, kind=1, y=y4 - 12)) == 2 + 21, 'ruby glyphs on line 4')
    # Line 5: a long ruby opens space, never overlapping neighbours.
    y5 = H_BODY + 5 * H_PITCH
    rei = one(a, '例', y=y5)
    rubies = G(a, kind=1, y=y5 - 12)
    require(len(rubies) == 13, f'long ruby {len(rubies)}')
    left, right = min(g['x'] for g in rubies), max(g['x'] + g['w'] for g in rubies)
    i = one(a, 'い', y=y5)
    no = one(a, 'の', y=y5)
    require(left < rei['x'] and right > rei['x'] + 24, 'long ruby overhangs its base')
    require(i['x'] + 24 <= left and no['x'] >= right, f'overhang spaced: {i} {left} {right} {no}')
    gaps = sorted(g['x'] for g in rubies)
    require(all(b - a_ == 12 for a_, b in zip(gaps, gaps[1:])), 'long ruby packed at 12 px')
    # 2-3: the caret in ｜本気《マジ》 shows it as written.
    for snap in s[1:3]:
        bar = one(snap, 0xff5c, y=H_BODY)
        require(bar['color'] == CYAN and G(snap, 'マ', y=H_BODY, kind=0), 'notation under the caret')
        require(not G(snap, 'マ', kind=1), 'no ruby while editing it')
        require(G(snap, 'か', y=H_RUBY, kind=1), 'other spans stay collapsed')
    require(s[1]['state']['off'] == 3 and s[2]['state']['off'] == 9, 'caret offsets in the notation')
    require(not G(s[3], 0xff5c, y=H_BODY) and G(s[3], 'マ', kind=1), 'collapsed again at the line end')
    require(s[4]['state']['line'] == 7 and s[4]['state']['off'] == 0, 'C-END')
    e = s[5]
    require(G(e, '《', y=H_BODY) and not G(e, kind=3), 'emphasis shown as written')
    require(one(e, '傍', y=H_BODY)['color'] == MAGENTA, 'written emphasis keeps its color')
    t = s[6]
    require(one(t, '（', y=H_BODY)['color'] == CYAN and G(t, 'と', y=H_BODY, kind=0), 'bracket ruby as written')
    return len(s)


def scenario_vertical(r, f):
    s = r.run(f['vert.txt'], 'novel_vertical.keys', 'vertical')
    v, h = s[0], s[1]
    require(v['state']['vertical'] == 1 and h['state']['vertical'] == 0 and s[2]['state']['vertical'] == 1, 'C-t toggles')
    for label, snap in zip('123456', s):
        check_ink(snap, f'vertical/{label}')
        check_frame(snap, f'vertical/{label}')
    # Rubies to the right of their bases.
    hon, ki = one(v, '本', x=V_BODY), one(v, '気', x=V_BODY)
    ma = one(v, 'マ', kind=1)
    require(ma['x'] == hon['x'] + 24 and hon['y'] <= ma['y'] < ki['y'] + 24, f'vertical ruby {hon} {ma}')
    require(one(v, '東')['color'] == MAGENTA, 'vertical colors')
    dots = G(v, kind=3)
    require(len(dots) == 2 and all(d['x'] == V_BODY + 24 for d in dots), f'vertical emphasis right of base {dots}')
    im, hi = image(v), image(h)
    # Turned half-width run, upright tate-chu-yoko pair, rotated dash, punctuation.
    A, B, C = one(v, 'A'), one(v, 'B'), one(v, 'C')
    require((A['w'], A['h']) == (24, 12) and B['y'] == A['y'] + 12 and C['y'] == B['y'] + 12 and A['x'] == C['x'],
            f'ABC turned {A} {B} {C}')
    one_, two = one(v, '1'), one(v, '2')
    require(one_['y'] == two['y'] and two['x'] == one_['x'] + 12 and one_['h'] == 24, f'12 side by side {one_} {two}')
    X = one(v, 'X')
    require((X['x'] - one_['x']) == 6 and X['w'] == 12, f'single letter centred {X}')
    dash = one(v, 'ー')
    bb = im.ink_bbox(dash['x'], dash['y'], 24, 24, PAL[BG])
    require(bb and bb[3] - bb[1] > 2 * (bb[2] - bb[0]), f'ー vertical {bb}')
    hd = one(h, 'ー')
    bb = hi.ink_bbox(hd['x'], hd['y'], 24, 24, PAL[BG])
    require(bb and bb[2] - bb[0] > 2 * (bb[3] - bb[1]), f'ー horizontal {bb}')
    maru = one(v, '。')
    bb = im.ink_bbox(maru['x'], maru['y'], 24, 24, PAL[BG])
    require(bb and bb[0] >= maru['x'] + 12 and bb[3] < maru['y'] + 12, f'。 upper right {bb}')
    hm = one(h, '。', y=H_BODY + 6 * H_PITCH)
    bb = hi.ink_bbox(hm['x'], hm['y'], 24, 24, PAL[BG])
    require(bb and bb[2] < hm['x'] + 12 and bb[1] >= hm['y'] + 12, f'。 lower left horizontally {bb}')
    kakko = one(v, '《', x=V_BODY - V_PITCH * 2) if G(v, '《', x=V_BODY - V_PITCH * 2) else None
    if kakko:
        bb = im.ink_bbox(kakko['x'], kakko['y'], 24, 24, PAL[BG])
        require(bb[2] - bb[0] > bb[3] - bb[1], '《 turned in vertical')
    # Caret moves: DOWN = next character, LEFT = next column, RIGHT back.
    d, l, rr = s[3], s[4], s[5]
    require(d['state']['line'] == 0 and d['state']['off'] == 6, f'DOWN in vertical {d["state"]["off"]}')
    require(l['state']['caret'][1] == d['state']['caret'][1] - V_PITCH and l['state']['line'] == 1, f"LEFT to the next column {d['state']['caret']} {l['state']['caret']}")
    require(rr['state']['caret'][1] == d['state']['caret'][1] and rr['state']['line'] == 0, 'RIGHT back')
    state = (r.home / '.fdtp' / 'novel-state').read_text()
    require(f'v\t{f["vert.txt"].resolve()}' in state, f'state file {state!r}')
    again = r.run(f['vert.txt'], 'novel_vertical_again.keys', 'vertical-again')
    require(again[0]['state']['vertical'] == 1, 'vertical remembered per file')
    return len(s) + len(again)


def scenario_edit(r, f):
    s = r.run(f['edit.txt'], 'novel_edit.keys', 'edit')
    want = ['Helloabc\ndef\n', None, 'Helloabclo\ndef\n', '', 'Helloabclo\ndef\n', 'Helloabc\ndef\n', 'abc\ndef\n',
            'Helloabclo\ndef\n', 'Helloabcloef\n', 'Helloabclox\n\tyef\n', None, 'Helloabclox\n\tyef\n', 'Z\tyef\n']
    require(len(s) == len(want), f'edit snaps {len(s)}')
    for i, (snap, w) in enumerate(zip(s, want)):
        check_frame(snap, f'edit/{i + 1}')
        if w is not None:
            require(text(snap) == w, f'edit/{i + 1}: {text(snap)!r} != {w!r}')
    sel = s[1]
    require(sel['state']['selection'] and sel['state']['sel'] == [0, 3, 0, 5], 'shift selection')
    for ch in 'lo':
        g = [x for x in G(sel, ch, y=H_BODY) if x['x'] >= BODY_LEFT + 3 * 12]
        require(g and g[0]['kind'] == 2, f'selected {ch} reversed')
        require(image(sel).count(g[0]['x'], g[0]['y'], 12, 24, WHITE) > 12 * 24 // 2, 'selection background')
    require(s[5]['state']['modified'] == 1 and s[6]['state']['modified'] == 0, 'undo back to unmodified')
    require(s[9]['state']['modified'] == 0 and '保存しました（UTF-8）' in notice(s[9]), 'saved')
    require(s[10]['state']['modified'] == 1 and s[10]['state']['lines'] == 18, 'thirty edits')
    require(s[11]['state']['modified'] == 0, 'thirty undos reach the saved text')
    require(f['edit.txt'].read_bytes() == b'Helloabclox\n\tyef\n', f'edit.txt {f["edit.txt"].read_bytes()!r}')
    return len(s)


def scenario_sjis(r, f):
    path = f['sjis.txt']
    s = r.run(path, 'novel_sjis.keys', 'sjis')
    require(s[0]['state']['encoding'] == 'Shift_JIS', 'Shift_JIS detected')
    require(G(s[0], 0x300a, y=H_BODY), 'caret at the start: notation shown as written')
    require(len(G(s[1], kind=1, y=H_RUBY)) == 3, 'ruby in Shift_JIS text')
    check_ink(s[1], 'sjis/2')
    first = '漢字《かんじ》テスト\r\n二行目\r\n追加'
    require('Shift_JIS' in notice(s[1]), f'sjis notice {notice(s[1])}')
    require(s[2]['state']['prompt'] == 7 and s[3]['state']['prompt'] == 0, 'asked for UTF-8')
    require(s[4]['state']['encoding'] == 'UTF-8' and s[4]['state']['modified'] == 0, 'saved as UTF-8')
    require(path.read_bytes() == (first + '🍣').encode('utf-8'), f'UTF-8 save {path.read_bytes()!r}')
    # Re-run the first half to check the Shift_JIS bytes themselves.
    path.write_bytes('漢字《かんじ》テスト\r\n二行目\r\n'.encode('cp932'))
    keys = r.work / 'sjis-only.keys'
    keys.write_text('C-END "追加" C-s ESC\n')
    r.run(path, keys, 'sjis-save')
    require(path.read_bytes() == first.encode('cp932'), f'Shift_JIS save {path.read_bytes()!r}')
    for name, encoding, prefix in (('euc.txt', 'euc_jp', b''), ('bom.txt', 'utf-8', codecs.BOM_UTF8)):
        original = f[name].read_bytes()
        e = r.run(f[name], 'novel_encodings.keys', name)
        require(e[0]['state']['encoding'] == ('EUC-JP' if encoding == 'euc_jp' else 'UTF-8'), f'{name} encoding')
        require(f[name].read_bytes() == original + 'あ'.encode(encoding), f'{name} saved {f[name].read_bytes()!r}')
        require(f[name].read_bytes().startswith(prefix), f'{name} BOM kept')
    bad = r.run(f['bad.txt'], 'novel_vertical_again.keys', 'bad')
    require('読めない字' in notice(bad[0]), 'invalid bytes reported')
    gone = r.run(r.work / 'fixtures', 'novel_vertical_again.keys', 'directory', expect=3)
    require(gone and gone[0].get('open') is False and '開けません' in gone[0]['notice'], 'directory refused')
    return len(s) + 6


def scenario_search(r, f):
    s = r.run(f['search.txt'], 'novel_search.keys', 'search')
    sels = [x['state']['sel'] for x in s[:4]]
    require(sels == [[0, 0, 0, 3], [0, 12, 0, 15], [1, 9, 1, 12], [0, 0, 0, 3]], f'search {sels}')
    require('先頭に戻って' in notice(s[3]), 'wrapped notice')
    require(s[4]['state']['prompt'] == 4 and s[4]['state']['sel'] == [0, 0, 0, 3], 'replace asks at the match')
    require('2 個置換しました' in notice(s[5]), f'replace count {notice(s[5])}')
    require(text(s[5]) == 'ねこと犬。猫が好き。\n犬よりねこ。\n', f'replaced {text(s[5])!r}')
    require(s[6]['state']['line'] == 1 and s[6]['state']['off'] == 0, 'go to line 2')
    require('行の番号' in notice(s[7]), 'bad line number')
    require(notice(s[8]) == '見つかりません' and s[8]['state']['query'] == '象', 'not found / prefill replaced')
    require(f['search.txt'].read_text() == 'ねこと犬。猫が好き。\n犬よりねこ。\n', 'search.txt saved')
    for snap in s:
        check_frame(snap, 'search')
    return len(s)


def scenario_rubyin(r, f):
    s = r.run(f['new.txt'], 'novel_rubyin.keys', 'rubyin')
    require(notice(s[0]) == '新しいファイルです', 'new file')
    require(text(s[1]) == '｜漢字《かんじ》', f'C-r after kanji {text(s[1])!r}')
    require(text(s[2]) == '｜漢字《かんじ》\na｜bc《x》', f'C-r on selection {text(s[2])!r}')
    require(text(s[3]) == '｜漢字《かんじ》\na｜bc《x》\n｜p《》', f'C-r alone {text(s[3])!r}')
    require(f['new.txt'].read_text() == '｜漢字《かんじ》\na｜bc《x》\n｜p《》', 'new file saved')
    return len(s)


def scenario_autosave(r, f):
    path = f['auto.txt']
    files = lambda: sorted(r.autosave.glob('*-auto.txt.txt'))
    s = r.run(path, 'novel_autosave1.keys', 'autosave1')
    require(len(s) == 2, 'autosave snaps')
    copies = files()
    require(len(copies) == 1 and copies[0].read_text() == '追記元の文\n', f'autosave copy {copies}')
    require(path.read_text() == '元の文\n', 'original untouched')
    s2 = r.run(path, 'novel_autosave2.keys', 'autosave2')
    require(s2[0]['state']['prompt'] == 9, 'recovery asked')
    require(text(s2[1]) == '追記元の文\n' and s2[1]['state']['modified'] == 1, 'recovered')
    require(s2[2]['state']['prompt'] == 6 and s2[3]['depth'] == 0, 'closed without saving')
    require(not files() and path.read_text() == '元の文\n', 'copy removed on a normal close')
    s3 = r.run(path, 'novel_autosave3.keys', 'autosave3')
    require(len(s3) == 2 and path.read_text() == 'z元の文\n' and not files(), 'saving removes the copy')
    return len(s) + len(s2) + len(s3)


def scenario_autosave_timing(r, f):
    """The first autosave snap (29 s) must not have a copy yet."""
    path = f['auto.txt']
    path.write_text('元の文\n')
    keys = r.work / 'autosave-early.keys'
    keys.write_text('"a" WAIT:29 SNAP ESC "n"\n')
    r.run(path, keys, 'autosave-early')
    # The ESC/n close removed any copy; check the one written at 29 s by a
    # second script that stops (crash-like) before closing.
    keys.write_text('"a" WAIT:29.5 SNAP\n')
    r.run(path, keys, 'autosave-early2')
    require(not sorted(r.autosave.glob('*-auto.txt.txt')), 'no copy at 29.5 s')
    return 2


def scenario_close(r, f):
    s = r.run(f['close.txt'], 'novel_close.keys', 'close')
    require(s[0]['state']['prompt'] == 6 and s[1]['state']['prompt'] == 0 and s[1]['depth'] == 1, 'Esc asks, Esc cancels')
    require(s[2]['depth'] == 0 and f['close.txt'].read_text() == 'xc\n', 'y saves and closes')
    return len(s)


def scenario_ime(r, f):
    s = r.run(f['ime.txt'], 'novel_ime.keys', 'ime', fake=True)
    require(s[0]['state']['ime'] == 'あ', 'input on')
    pre = s[1]
    require(text(pre) == 'ねこ\n', 'preedit not in the text')
    im = image(pre)
    for ch in 'にほん':
        g = one(pre, ch, y=H_BODY)
        require(im.count(g['x'], g['y'] + 22, g['w'] - 2, 2, WHITE) == (g['w'] - 2) * 2, f'preedit underline {ch}')
    cand = s[2]
    items = G(cand, kind=4)
    require(len(items) == 3 and all(one(cand, ch, kind=2) for ch in '日本'), f'candidates {items}')
    require(items[0]['color'] == BG and items[1]['color'] == WHITE, 'first candidate selected')
    imc = image(cand)
    require(imc.count(items[0]['x'], items[0]['y'], items[0]['w'], 24, WHITE) > 100, 'selected candidate reversed')
    require(G(s[3], kind=4)[1]['color'] == BG and G(s[3], '二', kind=2), 'next candidate')
    require(text(s[4]) == '二本ねこ\n' and not G(s[4], kind=4), f'committed {text(s[4])!r}')
    require(s[5]['state']['query'] == 'ねこ' and s[5]['state']['sel'] == [0, 6, 0, 12], 'search through the IME')
    require(s[6]['state']['ime'] == '切' and text(s[6]) == '二本ねこab\n', 'input off')
    require(f['ime.txt'].read_text() == '二本ねこab\n', 'ime.txt saved')
    for snap in s:
        check_ink(snap, 'ime')
    return len(s)


def scenario_scroll(r, f):
    s = r.run(f['scroll.txt'], 'novel_scroll.keys', 'scroll')
    st = [x['state'] for x in s]
    # 14 rows of 39 characters (horizontal), 26 columns of 21 (vertical) at
    # 80 x 25 (revision 1 item 29); line 5 is 100 characters: 3 rows, 5 columns.
    require((st[0]['line'], st[0]['top']) == (12, [12, 0]), f'PGDN {st[0]["line"]} {st[0]["top"]}')
    require((st[1]['line'], st[1]['top']) == (200, [187, 0]), f'C-END {st[1]["top"]}')
    require((st[2]['line'], st[2]['top']) == (0, [0, 0]), 'C-HOME')
    require((st[3]['line'], st[3]['off']) == (5, 117) and (st[4]['line'], st[4]['off']) == (5, 234), 'rows of a wrapped line')
    require((st[5]['line'], st[5]['off']) == (6, 0) and (st[6]['line'], st[6]['off']) == (5, 234), 'DOWN/UP across lines')
    require(st[7]['vertical'] == 1 and (st[7]['line'], st[7]['top']) == (22, [22, 0]), f'vertical PGDN {st[7]["line"]} {st[7]["top"]}')
    require((st[8]['line'], st[8]['top']) == (200, [175, 0]), f'vertical C-END {st[8]["top"]}')
    require(st[9]['vertical'] == 0 and st[9]['selection'] and st[9]['sel'] == [0, 0, 12, 0], f'S-PGDN {st[9]["sel"]}')
    for i, snap in enumerate(s):
        check_frame(snap, f'scroll/{i + 1}')
        require(snap['state']['caret'][0] == 1, f'scroll/{i + 1}: caret visible')
    rows = sorted({g['y'] for g in G(s[2], kind=0)})
    require(rows == [H_BODY + i * H_PITCH for i in range(H_ROWS)], f'{H_ROWS} rows {rows}')
    cols = sorted({g['x'] for g in G(s[7], kind=0) if g['w'] == 24 and g['h'] == 24})
    require(len(cols) == V_COLUMNS and cols[-1] == V_BODY and cols[0] == V_BODY - (V_COLUMNS - 1) * V_PITCH == BODY_LEFT, f'{V_COLUMNS} columns {cols[:3]}')
    long = [g for g in G(s[2], '長')]
    require(len([g for g in long if g['y'] == H_BODY + 5 * H_PITCH]) == 39, '39 characters per row')
    return len(s)


def expected_numbers(lines, top, width, page):
    """Line number (from 1) per row slot, None on the rows that continue a
    line; lines are all full-width characters except the digits."""
    out, (line, row) = [], top
    while len(out) < page and line < len(lines):
        px = sum(12 if ch.isascii() else 24 for ch in lines[line])
        rows = max(1, -(-px // width))
        out.append(line + 1 if row == 0 else None)
        row += 1
        if row >= rows:
            line, row = line + 1, 0
    return out


def numbers_by_slot(snap):
    st = snap['state']
    found = {}
    for g in G(snap, kind=5):
        if st['vertical']:
            pitch = max(V_PITCH, st['number_digits'] * 6)
            slot, rest = divmod(V_BODY + V_PITCH - pitch - (g['x'] - (pitch - g['w']) // 2), pitch)
            require(rest == 0 and g['y'] == V_NUM_Y and g['h'] == 12 and g['w'] == 6 * len(str(g['code'])),
                    f'vertical number box {g}')
        else:
            slot, rest = divmod(g['y'] - H_BODY, H_PITCH)
            require(rest == 0 and g['x'] + g['w'] == BODY_LEFT + 12 * st['number_digits'] and g['h'] == 24, f'horizontal number box {g}')
        require(slot not in found, f'two numbers in slot {slot}')
        require(image(snap).count(g['x'], g['y'], g['w'], g['h'], CYAN) >= 3 * len(str(g['code'])),
                f'number {g["code"]} not CYAN')
        found[slot] = g['code']
    return found


def scenario_numbers(r, f):
    lines = f['numbers.txt'].read_text().split('\n')
    s = r.run(f['numbers.txt'], 'novel_numbers.keys', 'numbers')
    require(len(s) == 8, f'numbers snaps {len(s)}')
    st = [x['state'] for x in s]
    require([x['numbers'] for x in st] == [0, 1, 1, 1, 0, 1, 1, 1], f'C-l toggles {[x["numbers"] for x in st]}')
    require([x['vertical'] for x in st] == [0, 0, 0, 1, 1, 1, 1, 0], 'C-t with numbers')
    for i, snap in enumerate(s):
        label = f'numbers/{i + 1}'
        check_ink(snap, label)
        check_frame(snap, label)
        im, x = image(snap), snap['state']
        # The frame's top line: CYAN at y 11-12, nothing just below it but the
        # line after the horizontal numbers (two dots, ┬).
        joint = 2 if x['numbers'] and not x['vertical'] else 0
        # (Under the numbers' line, and right of the name in the top line.)
        require(im.count(H_NUM_RULE_X, FRAME_TOP_Y + 2, 2, 1, CYAN) == joint and im.count(400, FRAME_TOP_Y + 2, BODY_RIGHT - 400, 1) == 0,
                f'{label}: the top line alone')
        body = G(snap, kind=0)
        if x['vertical']:
            chars = V_CHARS - (1 if x['numbers'] else 0)   # 20 under the numbers' line
            width, page = chars * 24, V_COLUMNS
            top = V_NUM_TOP if x['numbers'] else V_TOP
            require(min(g['y'] for g in body) == top, f'{label}: vertical text top {min(g["y"] for g in body)}')
            require(max(g['y'] + g['h'] for g in body) <= min(top + width, BODY_BOTTOM), f'{label}: vertical text bottom')
            # The numbers' line across the frame, joining its left and right lines.
            rule = im.count(FRAME_LEFT_X, V_NUM_RULE_Y, FRAME_RIGHT_X + 2 - FRAME_LEFT_X, 2, CYAN)
            require(rule == (2 * (FRAME_RIGHT_X + 2 - FRAME_LEFT_X) if x['numbers'] else 4) or (not x['numbers'] and rule < 200), f'{label}: number rule {rule}')
        else:
            width, page = BODY_RIGHT - (H_NUM_BODY if x['numbers'] else BODY_LEFT), H_ROWS
            left = H_NUM_BODY if x['numbers'] else BODY_LEFT
            require(min(g['x'] for g in body) == left, f'{label}: horizontal text left {min(g["x"] for g in body)}')
            require(max(g['x'] + g['w'] for g in body) <= left + width, f'{label}: horizontal text right')
            # From the frame's top line down to its bottom line (y 11 .. 540): ┬ ┴.
            rule = im.count(H_NUM_RULE_X, FRAME_TOP_Y, 2, FRAME_BOTTOM_Y + 2 - FRAME_TOP_Y, CYAN)
            # Without numbers only the frame's top and bottom lines cross there.
            require(rule == 2 * (FRAME_BOTTOM_Y + 2 - FRAME_TOP_Y) if x['numbers'] else rule <= 8, f'{label}: number rule {rule}')
            require(not x['numbers'] or im.count(H_NUM_RULE_X - 1, FRAME_TOP_Y + 2, 1, 520) == 0 and im.count(H_NUM_RULE_X + 2, FRAME_TOP_Y + 2, 1, 520) == 0,
                    f'{label}: number rule alone')
        want = expected_numbers(lines, tuple(x['top']), width, page)
        got = numbers_by_slot(snap)
        if x['numbers']:
            require(got == {k: n for k, n in enumerate(want) if n}, f'{label}: numbers {got} want {want}')
        else:
            require(not got, f'{label}: numbers while off')
    # Folding: 50 characters are 39 + 11 without numbers, 36 + 14 with them,
    # 20 + 20 + 10 in vertical (under the numbers' line); the rows that
    # continue line 2 carry no number.
    for snap, per in ((s[0], 39), (s[1], 36)):
        rows = sorted({g['y'] for g in G(snap, 'あ')})
        counts = [len(G(snap, 'あ', y=y)) for y in rows]
        require(counts == [per, 50 - per], f'line 2 folded {counts}')
    cols = sorted({g['x'] for g in G(s[3], 'あ')}, reverse=True)
    require([len(G(s[3], 'あ', x=c)) for c in cols] == [20, 20, 10], 'vertical 20 per column with numbers')
    require(numbers_by_slot(s[1]) == {0: 1, 1: 2, 3: 3, **{k: k for k in range(4, 14)}}, f'first page {numbers_by_slot(s[1])}')
    # 14 rows a page: PGDN puts line 14 (three rows of 長) on top.
    require(st[2]['top'] == [13, 0] and numbers_by_slot(s[2])[0] == 14 and 1 not in numbers_by_slot(s[2])
            and 2 not in numbers_by_slot(s[2]) and numbers_by_slot(s[2])[3] == 15, f'PGDN to line 14 {st[2]["top"]} {numbers_by_slot(s[2])}')
    v = numbers_by_slot(s[3])
    require(v.get(0) == 1 and v.get(1) == 2 and 2 not in v and 3 not in v and v.get(4) == 3, f'vertical first page {v}')
    require(st[6]['top'][0] > 0 and st[6]['top'][1] == 0 and numbers_by_slot(s[6])[0] == st[6]['top'][0] + 1,
            f'vertical PGDN {st[6]["top"]}')
    end = numbers_by_slot(s[6 + 1])
    require(max(end.values()) == len(lines) and st[7]['line'] == len(lines) - 1, f'C-END last number {end}')
    # Remembered per file, in the same state file as the writing direction.
    state = (r.home / '.fdtp' / 'novel-state').read_text()
    require(f'hn\t{f["numbers.txt"].resolve()}' in state, f'state file {state!r}')
    again = r.run(f['numbers.txt'], 'novel_numbers_again.keys', 'numbers-again')
    require(again[0]['state']['numbers'] == 1 and again[0]['state']['vertical'] == 0 and again[1]['state']['numbers'] == 0,
            'numbers remembered per file')
    state = (r.home / '.fdtp' / 'novel-state').read_text()
    require(f'h\t{f["numbers.txt"].resolve()}' in state and 'hn\t' not in state, f'state file after C-l {state!r}')
    return len(s) + len(again)


def scenario_number_growth(r, f):
    """The document count sets the gutter, which never shrinks until close."""
    path = r.work / 'number-growth.txt'
    # 36 あ and an a: one row of 876 dots beside four digits, a second row for
    # the a beside five (864 dots, revision 1 item 29).
    path.write_text('あ' * 36 + 'a\n' + '\n'.join(['文'] * 9998))
    keys = r.work / 'number-growth.keys'
    keys.write_text('C-l SNAP C-END RET SNAP C-z SNAP C-HOME SNAP C-t SNAP\n')
    s = r.run(path, keys, 'number-growth')
    require([x['state']['lines'] for x in s] == [9999, 10000, 9999, 9999, 9999],
            'inserting one line crosses the 9999 boundary and undo restores the count')
    require([x['state']['number_digits'] for x in s] == [4, 5, 5, 5, 5],
            'gutter grows on edit and stays wide on undo and scroll')
    require([len(G(s[0], 'あ', y=y)) for y in sorted({g['y'] for g in G(s[0], 'あ')})] == [36]
            and one(s[0], 'a')['y'] == G(s[0], 'あ')[0]['y'], 'four-digit gutter leaves 36 full-width characters and a half')
    require([len(G(s[3], 'あ', y=y)) for y in sorted({g['y'] for g in G(s[3], 'あ')})] == [36]
            and one(s[3], 'a')['y'] == G(s[3], 'あ')[0]['y'] + H_PITCH, 'gutter expansion recomputes horizontal wrapping')
    require(10000 in numbers_by_slot(s[1]).values(), 'the new 10000th line is drawn in full')
    for i, snap in enumerate(s):
        check_frame(snap, f'number-growth/{i}')
        numbers_by_slot(snap)
    require(min(g['x'] for g in G(s[3], kind=0)) == 116, 'five-digit body origin')
    # The line down column 6 (canvas x 109-110) from the frame's top line to its bottom line.
    require(image(s[3]).count(109, FRAME_TOP_Y, 2, FRAME_BOTTOM_Y + 2 - FRAME_TOP_Y, CYAN) == 2 * (FRAME_BOTTOM_Y + 2 - FRAME_TOP_Y),
            'five-digit gutter separator moves with the text')
    # A second open computes width from the whole file even at its first line.
    large = r.work / 'number-wide.txt'
    large.write_text('文\n' * 9999 + '末')
    keys.write_text('C-l SNAP C-END SNAP C-t SNAP\n')
    wide = r.run(large, keys, 'number-wide')
    require(all(x['state']['number_digits'] == 5 for x in wide), '10000-line document starts at five digits')
    require(10000 in numbers_by_slot(wide[2]).values(), 'vertical numbers preserve all digits')
    million = r.work / 'number-million.txt'
    million.write_text('\n' * 999999)
    keys.write_text('C-l C-t C-END SNAP C-HOME SNAP\n')
    huge = r.run(million, keys, 'number-million')
    require(all(x['state']['number_digits'] == 7 for x in huge), 'million-line document needs seven digits')
    require(1000000 in numbers_by_slot(huge[0]).values(), 'vertical millionth number is not truncated or reduced modulo')
    for snap in huge:
        nums = sorted(G(snap, kind=5), key=lambda g: g['x'])
        require(all(a['x'] + a['w'] <= b['x'] for a, b in zip(nums, nums[1:])),
                'seven-digit vertical numbers do not overlap adjacent columns')
        require(all(g['x'] >= BODY_LEFT and g['x'] + g['w'] <= V_BODY + V_PITCH for g in nums),
                'seven-digit vertical numbers fit on screen')
    return len(s) + len(wide) + len(huge)


def scenario_large(r, f, budget):
    path = f['large.txt']
    size = path.stat().st_size
    require(size > 1_000_000, f'large fixture {size}')
    s = r.run(path, 'novel_large.keys', 'large')
    elapsed = r.timings['large']
    require(s[1]['state']['line'] == LARGE_LINES and s[4]['state']['selection'] and s[4]['state']['line'] == 4000 and s[2]['state']['line'] == LARGE_LINES - 3 * H_ROWS, 'large navigation/search')
    data = path.read_text()
    require(data.startswith('先頭第00000行') and data.endswith('\n末尾') and len(data.encode()) == size + 12, 'large saved')
    require(elapsed < budget, f'1 MB scenario took {elapsed:.2f} s (budget {budget} s)')
    for snap in s:
        check_frame(snap, 'large')
    ll = r.run(f['longline.txt'], 'novel_longline.keys', 'longline')
    require(ll[1]['state']['line'] == 0 and text(ll[2]).startswith('頭あいう'), 'long line edits')
    return len(s) + len(ll)


WIDGET_NONE, WIDGET_NOTICE, WIDGET_CONFIRM, WIDGET_INPUT, WIDGET_INFO, WIDGET_ERROR, WIDGET_HELP = 0, 1, 2, 3, 5, 6, 7


def scenario_saveas(r, f):
    """Revision 1 3.18: Ctrl+Shift+s saves under another name and goes on
    editing that file; a new path is created by Ctrl+s."""
    d = f['saveas']
    src = d / 'src.txt'
    original = src.read_bytes()
    autos = lambda name: sorted(r.autosave.glob(f'*-{name}.txt'))
    s = r.run(src, 'novel_saveas1.keys', 'saveas1')
    require(len(s) == 3, f'saveas1 snaps {len(s)}')
    box = s[0]
    require(box['widget'] == WIDGET_INPUT and box['widget_label'] == '名前を付けて保存' and box['widget_text'] == 'src.txt'
            and box['state']['ask'] == 1, f'name box {box["widget"]} {box["widget_label"]!r} {box["widget_text"]!r}')
    im = image(box)
    require(im.count(32 + 7 * 12, 11 * 24, 66 * 12, 24, WHITE) > 66 * 12 * 24 // 2, 'white field on row 11')
    st = s[1]['state']
    require(st['name'] == 'out.txt' and st['path'] == str((d / 'out.txt').resolve()) and st['modified'] == 0
            and st['ask'] == 0 and s[1]['widget'] == WIDGET_NOTICE, f'adopted {st["name"]} {st["path"]}')
    require(notice(s[1]) == 'out.txt に保存しました（Shift_JIS）', f'save as notice {notice(s[1])!r}')
    require(st['encoding'] == 'Shift_JIS' and st['vertical'] == 1 and st['numbers'] == 1, 'encoding and modes kept')
    require((d / 'out.txt').read_bytes() == 'x漢字\r\n二行目\r\n'.encode('cp932'),
            f'out.txt keeps Shift_JIS and CRLF {(d / "out.txt").read_bytes()!r}')
    require(src.read_bytes() == original, 'the original file untouched')
    state = (r.home / '.fdtp' / 'novel-state').read_text()
    require(f'vn\t{(d / "out.txt").resolve()}' in state, f'state remembered for the new name {state!r}')
    require(len(autos('out.txt')) == 1 and not autos('src.txt'), f'autosave under the new name {list(r.autosave.iterdir())}')
    require(s[2]['state']['modified'] == 1, 'still editing after save as')
    again = r.run(d / 'out.txt', 'novel_saveas2.keys', 'saveas2')
    require(again[0]['state']['prompt'] == 9 and again[0]['state']['vertical'] == 1 and again[0]['state']['numbers'] == 1,
            'reopened: copy offered, direction and numbers remembered')
    require(again[1]['state']['prompt'] == 0 and not autos('out.txt'), 'declined copy removed on close')

    o = r.run(d / 'a.txt', 'novel_saveas3.keys', 'saveas3')
    require(len(o) == 8, f'saveas3 snaps {len(o)}')
    require(o[0]['widget'] == WIDGET_CONFIRM and o[0]['widget_label'] == 'exist.txt は既にあります。上書きしますか？',
            f'overwrite asked {o[0]["widget_label"]!r}')
    require(o[1]['widget'] == WIDGET_INPUT and o[1]['widget_text'] == 'exist.txt' and o[1]['state']['name'] == 'a.txt',
            f'N back to the name box {o[1]["widget"]} {o[1]["widget_text"]!r}')
    require(o[2]['state']['name'] == 'other.txt' and o[2]['widget'] == WIDGET_NOTICE, 'saved as other.txt')
    require(o[3]['state']['name'] == 'exist.txt' and o[3]['state']['modified'] == 0 and 'exist.txt に保存しました' in notice(o[3]),
            f'Y overwrote {notice(o[3])!r}')
    require(o[4]['widget'] == WIDGET_NOTICE and notice(o[4]) == '保存しました（UTF-8）', f'same file saved plainly {notice(o[4])!r}')
    require(o[5]['widget'] == WIDGET_CONFIRM and 'a.txt は既にあります' in o[5]['widget_label'], 'a.txt asks')
    require(o[6]['widget'] == WIDGET_NONE and o[6]['state']['name'] == 'exist.txt' and o[6]['state']['ask'] == 0, 'Esc at the question')
    require(o[7]['widget'] == WIDGET_NONE and o[7]['state']['ask'] == 0 and text(o[7]) == '12A\n', 'Esc in the name box')
    for name, want in (('a.txt', b'A\n'), ('other.txt', b'1A\n'), ('exist.txt', b'12A\n')):
        require((d / name).read_bytes() == want, f'{name}: {(d / name).read_bytes()!r}')
    history = (r.home / '.fdtp' / 'history').read_text()
    require('name\texist.txt' in history and 'name\tother.txt' in history, f'names in the history {history!r}')
    for i, snap in enumerate(o):
        check_frame(snap, f'saveas3/{i + 1}')

    e = r.run(d / 'b.txt', 'novel_saveas4.keys', 'saveas4')
    require(len(e) == 5, f'saveas4 snaps {len(e)}')
    require(e[0]['widget'] == WIDGET_ERROR and notice(e[0]).startswith('保存できません') and e[0]['state']['name'] == 'b.txt'
            and e[0]['state']['modified'] == 1, f'missing directory: error box {e[0]["widget"]} {notice(e[0])!r}')
    require(e[1]['widget'] == WIDGET_NONE and e[1]['depth'] == 1 and text(e[1]) == 'zB\n', 'error closed, still editing')
    require(not (d / 'nodir').exists(), 'no directory made')
    require(e[2]['widget'] == WIDGET_ERROR and 'ディレクトリには保存できません' in notice(e[2]), f'a directory {notice(e[2])!r}')
    require(e[3]['widget'] == WIDGET_HELP and e[3]['widget_label'] == '小説エディタのキー'
            and ['Ctrl+Shift+s', '名前を付けて保存'] in e[3]['widget_rows'], 'F1 help box (revision 1 item 25)')
    require(e[4]['widget'] == WIDGET_NONE and e[4]['depth'] == 1, 'help closed by Esc, the editor stays')
    require((d / 'b.txt').read_bytes() == b'B\n', 'b.txt untouched')

    # A path that does not exist yet: 「新しいファイルです」, Ctrl+s creates it;
    # in a directory that does not exist, an error box, then save as elsewhere.
    fresh = d / 'fresh.txt'
    keys = r.work / 'saveas-new.keys'
    keys.write_text('SNAP "abc" C-s SNAP\n')
    n = r.run(fresh, keys, 'saveas-new')
    require(notice(n[0]) == '新しいファイルです', f'new file {notice(n[0])!r}')
    require(fresh.read_bytes() == b'abc' and n[1]['state']['modified'] == 0, f'created by C-s {fresh.read_bytes()!r}')
    lost = d / 'missing' / 'new.txt'
    target = d / 'rescued.txt'
    keys.write_text(f'SNAP "abc" C-s SNAP ESC C-S-s C-u "{target}" RET SNAP\n')
    m = r.run(lost, keys, 'saveas-missing')
    require(notice(m[0]) == '新しいファイルです', 'new file in a missing directory')
    require(m[1]['widget'] == WIDGET_ERROR and notice(m[1]).startswith('保存できません'), f'C-s error box {notice(m[1])!r}')
    require(not (d / 'missing').exists(), 'no directory made by C-s')
    require(target.read_bytes() == b'abc' and m[2]['state']['name'] == 'rescued.txt', 'saved as an absolute path')
    return len(s) + len(again) + len(o) + len(e) + len(n) + len(m)


def scenario_saveas_ime(r, f):
    d = f['saveas']
    s = r.run(d / 'c.txt', 'novel_saveas_ime.keys', 'saveas-ime', fake=True)
    require(s[0]['widget'] == WIDGET_INPUT and s[0]['widget_text'] == '' and s[0]['state']['ime'] == 'あ', 'input on in the box')
    im = image(s[0])
    # The preedit ねこ (two full-width characters) on CYAN at the field's start, column 8.
    require(im.count(32 + 8 * 12, 11 * 24, 48, 24, CYAN) > 48 * 24 // 2, 'preedit drawn in the field')
    require((d / 'ねこ.txt').read_bytes() == b'qC\n' and s[1]['state']['name'] == 'ねこ.txt', 'saved under a Japanese name')
    return len(s)


def scenario_lossy(r, f):
    """Every explicit save path must ask before writing a binary/lossy file."""
    d = r.work / 'lossy'
    d.mkdir()
    keys = r.work / 'lossy.keys'
    count = 0
    warning = '元のバイト列には戻りません。保存しますか'

    def run(name, data, script, lossy=True):
        nonlocal count
        path = d / name
        path.write_bytes(data)
        # The existing load warning is an error box and consumes its closing key.
        keys.write_text(('ESC ' if lossy else '') + script + '\n')
        snaps = r.run(path, keys, 'lossy-' + name)
        count += len(snaps)
        return path, snaps

    def warned(s):
        require(s['widget'] == WIDGET_CONFIRM and s['widget_label'] == warning,
                f'lossy confirmation: {s["widget_label"]!r}')
        require(s['depth'] == 1, 'editor remains open at confirmation')
        check_frame(s, 'lossy confirmation')

    # CP932 accepts FF; NUL must still make this a binary save warning.
    # RET and unrelated keys cannot approve it, and N/Esc retain the bytes.
    for suffix, cancel in (('n', '"n"'), ('esc', 'ESC')):
        path, s = run('binary-' + suffix, b'\x00\xff',
                      f'C-s SNAP RET "q" SNAP {cancel} SNAP')
        warned(s[0]); warned(s[1])
        require(path.read_bytes() == b'\x00\xff', 'cancel keeps original binary bytes')
        require(s[2]['widget'] == WIDGET_NONE and s[2]['depth'] == 1,
                'cancel returns to editor')

    # A BOM makes replacement decoding and expected saved bytes unambiguous.
    raw = codecs.BOM_UTF8 + b'\x00\xff'
    want = codecs.BOM_UTF8 + b'x\x00\xef\xbf\xbd'
    path, s = run('plain', raw, '"x" C-s SNAP "y" SNAP')
    warned(s[0])
    require(path.read_bytes() == want and s[1]['state']['modified'] == 0,
            'Y permits plain lossy save')

    path, s = run('close-n', raw, '"x" ESC "y" SNAP "n" SNAP')
    warned(s[0])
    require(path.read_bytes() == raw and s[1]['depth'] == 1 and s[1]['state']['modified'] == 1,
            'declining loss after close-save keeps edited document and original bytes')
    path, s = run('close-y', raw, '"x" ESC "y" SNAP "y" SNAP')
    warned(s[0])
    require(path.read_bytes() == want and s[1]['depth'] == 0, 'approved close saves and closes')

    for overwrite in (False, True):
        target = d / ('existing.txt' if overwrite else 'new.txt')
        if overwrite:
            target.write_bytes(b'untouched')
        prefix = f'"x" C-S-s C-u "{target}" RET '
        if overwrite:
            prefix += 'SNAP "y" '
        path, s = run('as-n-' + str(overwrite), raw, prefix + 'SNAP "n" SNAP')
        warned(s[-2])
        require(path.read_bytes() == raw and (target.read_bytes() == b'untouched' if overwrite else not target.exists()),
                'save-as decline preserves source and destination')
        require(s[-1]['state']['name'] == path.name and s[-1]['state']['modified'] == 1,
                'save-as decline keeps original document identity')
        path, s = run('as-y-' + str(overwrite), raw, prefix + 'SNAP "y" SNAP')
        warned(s[-2])
        require(path.read_bytes() == raw and target.read_bytes() == want,
                'approved save-as writes destination only')
        require(s[-1]['state']['name'] == target.name, 'approved save-as adopts destination')

    # Invalid SJIS plus an emoji cannot be written in the detected encoding.
    # An initial Y must not bypass the warning on the subsequent UTF-8 retry.
    legacy = '漢字'.encode('cp932') + b'\x81'
    for mode in ('plain', 'close', 'as'):
        target = d / 'utf8-as.txt'
        begin = 'C-s' if mode == 'plain' else 'ESC "y"' if mode == 'close' else f'C-S-s C-u "{target}" RET'
        path, s = run('retry-n-' + mode, legacy,
                      f'"🍣" {begin} SNAP "y" SNAP "y" SNAP "n" SNAP')
        warned(s[0]); warned(s[2])
        require(s[1]['state']['prompt'] == {'plain': 7, 'close': 8, 'as': 10}[mode],
                'unrepresentable text offers UTF-8')
        require(path.read_bytes() == legacy and s[3]['depth'] == 1, 'UTF-8 retry decline preserves bytes')
        if mode == 'as':
            require(not target.exists(), 'UTF-8 retry decline does not create destination')
        path, s = run('retry-y-' + mode, legacy,
                      f'"🍣" {begin} SNAP "y" SNAP "y" SNAP "y" SNAP')
        warned(s[0]); warned(s[2])
        saved = target if mode == 'as' else path
        require(saved.read_bytes() == '🍣漢字�'.encode(), 'approved UTF-8 retry saves full replacement text')
        require(s[3]['depth'] == (0 if mode == 'close' else 1), 'UTF-8 retry follows close continuation')

    for encoding in ('utf-8', 'cp932'):
        original = '通常の文章\r\n'.encode(encoding)
        path, s = run('normal-' + encoding, original, '"x" C-s SNAP', lossy=False)
        require(s[0]['widget'] == WIDGET_NOTICE and s[0]['state']['modified'] == 0,
                'ordinary text saves without loss confirmation')
        require(path.read_bytes() == b'x' + original, 'ordinary encoding and newlines preserved')
    return count


def run_all(runner, f, budget):
    count = 0
    for name, fn in (('ruby', scenario_ruby), ('vertical', scenario_vertical), ('edit', scenario_edit),
                     ('sjis', scenario_sjis), ('search', scenario_search), ('rubyin', scenario_rubyin),
                     ('autosave', scenario_autosave), ('autosave-timing', scenario_autosave_timing),
                     ('close', scenario_close), ('ime', scenario_ime), ('scroll', scenario_scroll),
                     ('numbers', scenario_numbers), ('number-growth', scenario_number_growth), ('saveas', scenario_saveas), ('saveas-ime', scenario_saveas_ime),
                     ('lossy', scenario_lossy)):
        try:
            count += fn(runner, f)
        except Failure as error:
            raise Failure(f'{runner.label}/{name}: {error}')
    try:
        count += scenario_large(runner, f, budget)
    except Failure as error:
        raise Failure(f'{runner.label}/large: {error}')
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--no-sanitize', action='store_true')
    args = parser.parse_args()
    directory = args.build.resolve()
    variants = [('plain', directory, False, 3.0)]
    if not args.no_sanitize:
        variants.append(('asan', directory / 'novel-asan', True, 120.0))
    for label, build_dir, sanitize, budget in variants:
        harness, fake, unit = build(build_dir, sanitize)
        work = directory / 'novel-check' / label
        if work.exists():
            shutil.rmtree(work)
        (work / 'tmp').mkdir(parents=True)
        (work / 'unit').mkdir()
        r = subprocess.run([str(unit), str(work / 'unit')], capture_output=True, text=True,
                           env=dict(os.environ, FDTP_LANGUAGE='ja', ASAN_OPTIONS='halt_on_error=1:detect_leaks=0',
                                    UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))
        if r.returncode or 'runtime error' in r.stderr:
            raise Failure(f'{label}/unit:\n{r.stderr[-4000:]}')
        print(f'{label}: {r.stdout.strip()}')
        f = fixtures(work / 'fixtures')
        runner = Runner(work, harness, fake, label)
        snaps = run_all(runner, f, budget)
        slow = ', '.join(f'{k} {v:.2f}s' for k, v in sorted(runner.timings.items(), key=lambda kv: -kv[1])[:3])
        print(f'{label}: PASS {runner.runs} runs, {snaps} snapshots (slowest: {slow})')


if __name__ == '__main__':
    try:
        main()
    except (Failure, subprocess.CalledProcessError, OSError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
