#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Revision 1 §7 and §12 checks (docs/FDTP_REVISION1.md): floating boxes with
a dotted frame and a shadow, the white input field, the information box, the
overlay on pixel screens, and the input history file with its pull-down list.

Everything is written under BUILD/popup-check; $FDTP_HISTORY always points
there, and one run checks that the headless runner never writes $HOME/.fdtp.
tests/popup_harness.c covers what no command reaches yet (widgets_info) and
the history file's parsing boundaries; the headless scripts popup_*.keys
cover the real screens."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

from check import ERROR_BOX, PRESS_ANY_KEY, check_snapshot, notice, popup_rows, require, text

ROOT = Path(__file__).resolve().parents[1]
CYAN, NAVY, WHITE, BLACK, GREY, YELLOW = (0, 255, 255), (0, 0, 85), (255, 255, 255), (0, 0, 0), (128, 128, 128), (255, 255, 0)
BG_INDEX, WHITE_INDEX, CYAN_INDEX = 0, 2, 1


def ppm(path):
    data = Path(path).read_bytes()
    magic, wh, maxv, pixels = data.split(b'\n', 3)
    w, h = map(int, wh.split())
    require(magic == b'P6' and maxv == b'255' and (w, h) == (1024, 600) and len(pixels) == w * h * 3, f'{path}: not a 1024x600 P6')
    return pixels


def pixel(pixels, gx, gy):
    """Pixel of the 960x600 grid (the canvas starts it at x = 32)."""
    i = (gy * 1024 + 32 + gx) * 3
    return tuple(pixels[i:i + 3])


def build_harness(build):
    exe = build / 'popup-harness'
    sources = [ROOT / 'src' / f'{name}.c' for name in ('widgets', 'cells', 'raster', 'canvas', 'font24', 'keys', 'i18n')]
    cc = shlex.split(os.environ.get('CC', 'cc'))
    flags = ['-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Werror', '-g', '-I' + str(ROOT / 'src'), '-I' + str(build)]
    if os.environ.get('POPUP_SANITIZE'):
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=undefined']
    subprocess.run(cc + flags + [str(ROOT / 'tests/popup_harness.c')] + [str(s) for s in sources] + ['-o', str(exe)], check=True)
    return exe


class Runner:
    def __init__(self, build, base):
        self.build, self.base = build, base
        self.executable = build / ('fdtp-preview' if sys.platform == 'darwin' else 'fdtp')
        self.snapshots = 0

    def run(self, script, directory, env, prefix):
        source = (ROOT / 'tests' / script).read_text()
        labels = [line.split('#', 1)[1].strip() for line in source.splitlines() if 'SNAP #' in line]
        for old in prefix.parent.glob(prefix.name + '-*'):
            old.unlink()
        subprocess.run([str(self.executable), '--headless', '--script', str(ROOT / 'tests' / script), '--out', str(prefix),
                        '--time', '1700000000', str(directory)], check=True, cwd=ROOT.parent, env=env, stdout=subprocess.DEVNULL)
        files = sorted(prefix.parent.glob(prefix.name + '-*.cells'))
        require(len(files) == len(labels), f'{script}: {len(files)} snapshots for {len(labels)} labels')
        self.snapshots += len(files)
        print(f'  {script}: {len(files)} snapshots')
        return {label: dict(check_snapshot(f), ppm=f.with_suffix('.ppm')) for label, f in zip(labels, files)}


def fixture(base):
    d = base / 'files'
    if d.exists():
        shutil.rmtree(d)
    for name in ('alpha', 'beta'):
        (d / name).mkdir(parents=True)
    for name in ('a.txt', 'b.txt'):
        (d / name).write_text(name + '\n')
    return d


def frame_and_shadow(snap, label):
    """The box's frame pixels are CYAN two dots wide, its inside navy, and a
    one-cell shadow (black ground, grey or black only) lies right and below."""
    require(len(snap['boxes']) == 1, f'{label}: one box, got {snap["boxes"]}')
    col, row, w, h, rule, kind = snap['boxes'][0]
    require(kind == 3, f'{label}: framed with a shadow')
    p = ppm(snap['ppm'])
    left, right, top, bottom = col * 12 + 5, (col + w - 1) * 12 + 5, row * 24 + 11, (row + h - 1) * 24 + 11
    mid_x, mid_y = (col + w // 2) * 12 + 3, (row + h // 2) * 24 + 12
    for x, y in [(mid_x, top), (mid_x, top + 1), (mid_x, bottom), (mid_x, bottom + 1), (left, mid_y), (left + 1, mid_y), (right, mid_y), (right + 1, mid_y)]:
        require(pixel(p, x, y) == CYAN, f'{label}: frame pixel {x},{y} is {pixel(p, x, y)}')
    for x, y in [(mid_x, top - 1), (mid_x, top + 2), (left - 1, mid_y), (left + 2, mid_y), (left, top), (right + 1, bottom + 1)]:
        require(pixel(p, x, y) != CYAN, f'{label}: {x},{y} outside the two-dot line / round corner')
    require(pixel(p, (col + 1) * 12 + 1, row * 24 + 1) == NAVY, f'{label}: inside is navy')
    shadow = [(gx, gy) for gy in range((row + 1) * 24, (row + h + 1) * 24) for gx in range((col + w) * 12, (col + w + 1) * 12)]
    shadow += [(gx, gy) for gy in range((row + h) * 24, (row + h + 1) * 24) for gx in range((col + 1) * 12, (col + w) * 12)]
    colours = {pixel(p, x, y) for x, y in shadow}
    require(BLACK in colours and all(c[0] == c[1] == c[2] and c[0] <= 128 for c in colours), f'{label}: shadow colours {sorted(colours)[:6]}')
    require(pixel(p, (col + w) * 12 + 1, row * 24 + 1) != BLACK, f'{label}: the shadow starts one row down')
    require(pixel(p, (col + w + 1) * 12 + 1, (row + 1) * 24 + 1) != BLACK, f'{label}: the shadow is one column wide')
    return col, row, w, h, rule


def check_boxes(r, base):
    files = fixture(base)
    env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(r.build / 'tmp'), TZ='UTC', HOME=str(base / 'home'), FDTP_HISTORY=str(base / 'boxes-history'))
    (base / 'home').mkdir(exist_ok=True)
    s = r.run('popup_boxes.keys', files, env, base / 'boxes')
    require(not s['initial']['boxes'], 'no box at first')
    col, row, w, h, _ = frame_and_shadow(s['confirm'], 'confirm')
    require((row, h) == (11, 3) and col == (80 - w) // 2 and notice(s['confirm']) == 'FDTP を終了しますか？ (Y/N)', f'confirm box {s["confirm"]["boxes"]} {notice(s["confirm"])!r}')
    require(any(c[1] == 3 and c[0] != 32 for c in s['confirm']['rows'][12]), 'confirm text yellow')
    # The list outside the box stays visible: row 6 (the ..) and the band.
    require(s['confirm']['rows'][6] == s['initial']['rows'][6] and s['confirm']['rows'][24] == s['initial']['rows'][24], 'list outside the box unchanged')
    frame_and_shadow(s['choice'], 'choice')
    rows = [t for _, t in popup_rows(s['choice'])]
    require(rows[0] == '並べ替え' and rows[1].split() == ['名前', '拡張子', 'サイズ', '日付', '長さ', '並べない'], f'choice rows {rows!r}')
    col, row, w, h, _ = frame_and_shadow(s['input'], 'input')
    require((col, row, w, h) == (5, 9, 70, 4), f'input box {s["input"]["boxes"]}')
    field = s['input']['rows'][11][7:73]
    require(all(c[2] == WHITE_INDEX for c in field[:1] + field[2:]) and field[1][2] == BG_INDEX, 'white field, caret reversed at its start')
    require(text(s['input'], 10, 7, 73).startswith('コピー先') and text(s['input'], 10, 7, 73).rstrip().endswith('↓ 履歴  F8 ツリー'), 'label, history and tree hints')
    p = ppm(s['input']['ppm'])
    require(pixel(p, 7 * 12 + 1, 11 * 24 + 1) == WHITE and pixel(p, 8 * 12 + 1, 11 * 24 + 1) == NAVY, 'field white, caret navy (pixels)')
    frame_and_shadow(s['error'], 'error')
    require(s['error']['widget'] == ERROR_BOX and notice(s['error']) == 'ディレクトリを開けません', notice(s['error']))
    require(text(s['error'], s['error']['boxes'][0][1] + 2).strip() == PRESS_ANY_KEY, 'press-any-key line')
    require(not s['closed']['boxes'] and s['closed']['widget'] == 0, 'ESC closes the error box')
    # Every row drawn again: the screen is the same as before any box, in cells and pixels.
    require(s['back_at_top']['rows'] == s['initial']['rows'], 'cells restored after the boxes')
    require(ppm(s['back_at_top']['ppm']) == ppm(s['initial']['ppm']), 'pixels restored after the boxes (no frame or shadow left)')


def history_file(path):
    lines = [l for l in path.read_text().splitlines() if l and not l.startswith('#')]
    return [tuple(l.split('\t', 1)) for l in lines]


def list_rows(snap):
    col, row, w, h, rule, kind = snap['boxes'][0]
    return [text(snap, y, 7, 73).strip() for y in range(rule + 1, row + h - 1)]


def check_history(r, base):
    files = fixture(base)
    history = base / 'history'
    if history.exists():
        history.unlink()
    home = base / 'home'
    shutil.rmtree(home, ignore_errors=True)
    home.mkdir()
    env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(r.build / 'tmp'), TZ='UTC', HOME=str(home), SHELL='/bin/sh')
    env.pop('FDTP_HISTORY', None)
    # Headless without $FDTP_HISTORY: kept in memory, nothing under $HOME.
    r.run('popup_history1.keys', files, env, base / 'nohistory')
    require(not (home / '.fdtp').exists(), 'headless run wrote $HOME/.fdtp')
    files = fixture(base)
    env['FDTP_HISTORY'] = str(history)
    s = r.run('popup_history1.keys', files, env, base / 'h1')
    require((files / 'alpha' / 'b.txt').read_text() == 'b.txt\n', 'c copied b.txt into alpha')
    require(history_file(history) == [('destination', 'alpha'), ('path', '..'), ('path', 'beta'), ('path', 'alpha')],
            f'history after the first run: {history_file(history)}')
    s = r.run('popup_history2.keys', files, env, base / 'h2')
    opened = s['opened']
    require(opened['boxes'][0][3] == 4 and 'beta' not in notice(opened), 'list closed at first')
    col, row, w, h, rule = frame_and_shadow(s['list'], 'list')
    require((h, rule) == (8, 12) and list_rows(s['list']) == ['..', 'beta', 'alpha'], f'list {s["list"]["boxes"]} {list_rows(s["list"])}')
    require(s['list']['rows'][13][8][2] == CYAN_INDEX and s['list']['rows'][14][8][2] == BG_INDEX, 'first entry chosen')
    p = ppm(s['list']['ppm'])
    require(pixel(p, 40 * 12, 12 * 24 + 11) == CYAN, 'rule between field and list')
    require(s['chosen']['boxes'][0][3] == 4 and text(s['chosen'], 11, 8, 72).strip() == 'beta', 'RET puts the entry in the field')
    require(text(s['moved'], 3, 7, 51).rstrip().endswith('/beta'), 'second RET goes there')
    require(list_rows(s['filtered']) == ['alpha'], f'typed a: {list_rows(s["filtered"])}')
    require(not s['cancelled']['boxes'], 'Esc closes the list, then the input')
    require(list_rows(s['destinations']) == ['alpha'] and text(s['destinations'], 10, 7, 73).startswith('コピー先'), 'destinations are their own kind')
    require(history_file(history) == [('destination', 'alpha'), ('path', 'beta'), ('path', '..'), ('path', 'alpha')],
            f'history after the second run: {history_file(history)}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    build = args.build.resolve()
    base = build / 'popup-check'
    shutil.rmtree(base, ignore_errors=True)
    base.mkdir(parents=True)
    (build / 'tmp').mkdir(exist_ok=True)
    harness = build_harness(build)
    subprocess.run([str(harness), str(base)], check=True, env=dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(build / 'tmp')))
    r = Runner(build, base)
    check_boxes(r, base)
    check_history(r, base)
    print(f'revision 1 popup checks passed: harness, {r.snapshots} snapshots, history file across runs')


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError, subprocess.CalledProcessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
