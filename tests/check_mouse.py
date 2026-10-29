#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""FDTM's mouse on the filer (docs/FDTP_MACOS.md), through the headless
script's CLICK:COL,ROW[,CMD|SHIFT|POINT] (filer_mouse, as the Mac window
calls it): a click selects alone, Cmd+click adds or removes, Shift+click
marks a range, directories are never marked, nothing happens under a box.

Run python3 -B tests/check_mouse.py [--build BUILD]. Everything stays in
BUILD/mouse-check.
"""
import argparse
import os
from pathlib import Path
import shutil

from check import require, text
from check_popup import Runner

NAMES = ['..', 'dir1', 'a', 'b', 'c', 'd', 'e'] + [f'z{i:02d}' for i in range(20)]


def entries(snap):
    """(name, marked, cursor) of every entry shown, slot by slot."""
    found = []
    for left in (0, 40):
        for y in range(6, 23):
            name = text(snap, y, left + 2, left + 10).strip()
            if not name:
                continue
            found.append((name, text(snap, y, left + 1, left + 2) == '*', snap['rows'][y][left + 4][2] != 0))
    return found


def marks(snap):
    return [n for n, m, _ in entries(snap) if m]


def cursor(snap):
    names = [n for n, _, c in entries(snap) if c]
    require(len(names) == 1, f'one cursor: {names}')
    return names[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=Path(__file__).resolve().parents[1] / 'build')
    build = parser.parse_args().build.resolve()
    base = build / 'mouse-check'
    shutil.rmtree(base, ignore_errors=True)
    files = base / 'files'
    (files / 'dir1').mkdir(parents=True)
    for name in NAMES[2:]:
        (files / f'{name}.txt').write_text(name + '\n')
    (base / 'home').mkdir()
    env = dict(os.environ, HOME=str(base / 'home'), TZ='UTC', FDTP_LANGUAGE='ja', FDTP_HISTORY='',
               FDTP_RC_SYSTEM='', FDTP_RC_USER=str(base / 'fdtprc'), FDTP_POMERA_CHECK='', FDTP_POMERA_SEEN='')
    s = Runner(build, base).run('mouse.keys', files, env, base / 'snap')
    require([n for n, _, _ in entries(s['start'])][:7] == NAMES[:7], f'layout: {entries(s["start"])}')
    require(cursor(s['select_a']) == 'a' and not marks(s['select_a']), 'a click moves the cursor there')
    require(marks(s['cmd_two']) == ['c', 'e'] and cursor(s['cmd_two']) == 'e', f'Cmd+click adds: {marks(s["cmd_two"])}')
    require(marks(s['cmd_off']) == ['e'] and cursor(s['cmd_off']) == 'c', 'Cmd+click again removes')
    require(not marks(s['plain_clears']) and cursor(s['plain_clears']) == 'b', 'a plain click clears the marks')
    require(marks(s['range']) == ['b', 'c', 'd'] and cursor(s['range']) == 'd', f'Shift+click: {marks(s["range"])}')
    require(marks(s['range_up']) == ['a', 'b'] and cursor(s['range_up']) == '..', f'the range from the last click, directories left: {marks(s["range_up"])}')
    require(marks(s['dir_cmd']) == ['a', 'b'] and cursor(s['dir_cmd']) == 'dir1', 'Cmd+click on a directory does not mark it')
    require(cursor(s['second_column']) == 'z10' and not marks(s['second_column']), 'the second list of the page')
    require(s['empty']['rows'] == s['second_column']['rows'], 'a click on no entry changes nothing')
    require(s['boxed']['widget'] != 0 and cursor(s['unboxed']) == 'z10', 'no click under an open box')
    require(cursor(s['point']) == 'dir1', 'POINT only moves the cursor')
    require('/files/dir1 ' in text(s['entered'], 3, 0, 52) and [n for n, _, _ in entries(s['entered'])] == ['..'],
            'Return after a click enters the directory (a double click)')
    # A line-23 notice ("完了 1 件" after a drop) is no box: a click still
    # works and clears it (a drop onto such a window was refused).
    require(s['notice']['widget'] == 1 and '完了' in text(s['notice'], 23), f'the copy notice: {text(s["notice"], 23)}')
    require((files / 'dir1' / 'a.txt').exists(), 'a.txt copied by c')
    require(s['after_notice']['widget'] == 0 and cursor(s['after_notice']) == 'b', 'a click under the notice works and clears it')
    print('mouse: OK')


if __name__ == '__main__':
    main()
