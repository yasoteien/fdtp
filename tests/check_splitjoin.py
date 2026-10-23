#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Revision 1 item 37: real byte split/join and ordered floating boxes.

python3 -B tests/check_splitjoin.py --build build [--size 85x25]
All fixtures, shell restoration results, snapshots and temporary files stay in BUILD.
"""
import argparse
import json
import os
from pathlib import Path
import re
import resource
import signal
import shutil
import subprocess
import sys

from check import require
from check_wide import Snap

ROOT = Path(__file__).resolve().parents[1]
SETTLE = ' WAIT:0 ' * 128


def quote(value):
    return json.dumps(str(value), ensure_ascii=False)


def select(name):
    return 'C-s ' + quote(name) + ' ESC '


def enter(value):
    return 'C-u ' + quote(value) + ' RET '


def marks(names):
    return ''.join(select(name) + 'TAB ' for name in names)


def rendered(snap):
    return '\n'.join(snap.text(row) for row in range(snap.nrows))


def input_text(snap):
    return ''.join(chr(cell[0]) for row in snap.rows for cell in row
                   if cell[1] == 0 and cell[2] == 2 and cell[3] != 2).strip()


class Runner:
    def __init__(self, build, size):
        self.build, self.size = build, size
        self.cols, self.rows = map(int, size.split('x'))
        self.base = build / ('splitjoin-check-' + size)
        shutil.rmtree(self.base, ignore_errors=True)
        self.base.mkdir(parents=True)
        self.exe = build / ('fdtp-preview' if sys.platform == 'darwin' else 'fdtp')
        self.env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(build / 'tmp'), TZ='UTC', SHELL='/bin/sh',
                        HOME=str(self.base / 'home'), FDTP_RC_SYSTEM='',
                        FDTP_RC_USER=str(self.base / 'none.fdtprc'))
        (self.base / 'home').mkdir()
        self.count, self.snapshots = 0, 0

    def case(self, name):
        home = self.base / name
        (home / 'src').mkdir(parents=True)
        return home, home / 'src'

    def run(self, home, keys, settle=True, output_limit=None):
        tokens = re.compile(r'"(?:[^"\\]|\\.)*"|#[^\n]*|\bRET\b|\by\b')
        if settle:
            keys = tokens.sub(lambda m: m[0] + SETTLE if m[0] in ('RET', 'y') else m[0], keys)
        script = home / 'run.keys'
        script.write_text(keys + SETTLE + '\nSNAP # result\n')
        prefix = home / 'screen'
        for old in home.glob('screen-*'):
            old.unlink()
        def limit_output():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (output_limit, output_limit))

        subprocess.run([str(self.exe), '--headless', '--size', self.size, '--script', str(script),
                        '--out', str(prefix), '--time', '1700000000', str(home / 'src')],
                       cwd=ROOT.parent, env=self.env, check=True, stdout=subprocess.DEVNULL, timeout=60,
                       preexec_fn=limit_output if output_limit else None)
        snaps = [Snap(p) for p in sorted(home.glob('screen-*.cells'))]
        require(snaps, f'{home.name}: no snapshots')
        for snap in snaps:
            require((snap.cols, snap.nrows) == (self.cols, self.rows), f'{home.name}: screen dimensions')
            for row in snap.rows:
                require(len(row) == self.cols, f'{home.name}: row width')
                for x, cell in enumerate(row):
                    require(len(cell) == 4, f'{home.name}: cell shape')
                    if cell[3] == 1:
                        require(x + 1 < self.cols and row[x + 1] == cell[:3] + [2], f'{home.name}: wide glyph')
                    if cell[3] == 2:
                        require(x > 0 and row[x - 1] == cell[:3] + [1], f'{home.name}: orphan glyph')
        self.count += 1
        self.snapshots += len(snaps)
        print(f'  {home.name}: {len(snaps)} snapshots')
        return snaps

    def box(self, snap, label):
        boxes = [b for b in snap.boxes() if b[5] & 2]
        require(boxes, label + ': floating box with shadow')
        for x, y, w, h, _, _ in boxes:
            require(0 <= x and 0 <= y and x + w <= self.cols and y + h <= self.rows,
                    label + ': box in bounds')
            for pixel_y in range((y + 1) * 24, (y + h - 1) * 24, 5):
                require(snap.gpx(x * 12 + 5, pixel_y) == (0, 255, 255) and
                        snap.gpx((x + w) * 12 - 7, pixel_y) == (0, 255, 255),
                        label + ': visible frame sides')

    def split(self):
        for index, source in enumerate(('data.bin', "-space and 'quote.bin")):
            home, src = self.case('split_script_' + str(index))
            data = bytes(range(256)) * 10 + b'last\x00byte'
            (src / source).write_bytes(data)
            prefix = "-part ' one" if index else 'part'
            snaps = self.run(home, select(source) + 'M-n SNAP # size\n' + enter('1K') +
                             'SNAP # prefix\n' + enter(prefix) + 'SNAP # confirm\ny SNAP # script\ny ')
            require('10M' in rendered(snaps[0]), 'split: default size is 10M')
            for snap in snaps[:4]:
                self.box(snap, 'split input/confirmation')
            require('3' in rendered(snaps[2]), 'split: expected part count shown')
            parts = sorted(src.glob(prefix + '.[0-9][0-9][0-9]'))
            require([p.stat().st_size for p in parts] == [1024, 1024, len(data) - 2048], 'split: sizes/count')
            require(b''.join(p.read_bytes() for p in parts) == data, 'split: exact binary bytes')
            scripts = list(src.glob('*.join.sh'))
            require(len(scripts) == 1, 'split: one restoration script')
            # Keep only the parts and generated script, so cat must really restore.
            (src / source).unlink()
            subprocess.run(['/bin/sh', str(scripts[0])], cwd=src, env=self.env, check=True, timeout=10)
            restored = [p for p in src.iterdir() if p not in parts and p != scripts[0]]
            require(len(restored) == 1 and restored[0].name == prefix and restored[0].read_bytes() == data,
                    'generated script restores names with spaces, quote and leading dash')

        for length in (0, 15, 1024):
            home, src = self.case('split_unneeded_' + str(length))
            (src / 'source').write_bytes(b'x' * length)
            snaps = self.run(home, select('source') + 'M-n ' + enter('1K'))
            require(any(word in rendered(snaps[-1]) for word in ('不要', '必要', '小さ', '以下', '空')),
                    'split: unnecessary split notice')
            require([p.name for p in src.iterdir()] == ['source'], 'unnecessary split creates nothing')

        home, src = self.case('split_overwrite_no')
        (src / 'source').write_bytes(b'x' * 2500)
        (src / 'part.002').write_bytes(b'preserve')
        before = {p.name: p.read_bytes() for p in src.iterdir()}
        snaps = self.run(home, select('source') + 'M-n ' + enter('1K') + enter('part') + 'y n ')
        require(snaps[-1].data['depth'] == 1, 'split: overwrite refusal ends the operation')
        require({p.name: p.read_bytes() for p in src.iterdir()} == before,
                'split: declining overwrite aborts all output before writing')

        home, src = self.case('split_cancel')
        (src / 'source').write_bytes(b'x' * 2500)
        self.run(home, select('source') + 'M-n ' + enter('1K') + enter('part') + 'n ')
        require([p.name for p in src.iterdir()] == ['source'], 'split: declining count confirmation creates nothing')

        home, src = self.case('split_no_script')
        (src / 'source').write_bytes(b'x' * 2500)
        self.run(home, select('source') + 'M-n ' + enter('1K') + enter('part') + 'y n ')
        require(len(list(src.glob('part.[0-9][0-9][0-9]'))) == 3 and not list(src.glob('*.join.sh')),
                'split: script default off / declined')

    def join(self):
        for reorder, expected in (('d', b'BAC'), ('DOWN u', b'BAC'), ('S-DOWN', b'BAC'),
                                  ('DOWN S-UP', b'BAC'), ('', b'ABC')):
            home, src = self.case('join_' + (reorder.replace(' ', '_') or 'default'))
            names = ['a.001', 'a.002', 'a.003']
            for name, value in zip(names, (b'A', b'B', b'C')):
                (src / name).write_bytes(value)
            snaps = self.run(home, marks(reversed(names)) + 'M-h SNAP # order\n' + reorder +
                             ' RET SNAP # output\n' + enter('result'))
            self.box(snaps[0], 'join order')
            require('a.001' in rendered(snaps[0]) and 'a.003' in rendered(snaps[0]), 'order names')
            require((src / 'result').read_bytes() == expected, 'join: initial name sort and reorder ' + reorder)
            require(all((src / n).read_bytes() == v for n, v in zip(names, (b'A', b'B', b'C'))),
                    'join preserves sources')
            if not reorder:
                require(input_text(snaps[1]) == 'a', 'join strips .NNN for initial output name')

        home, src = self.case('join_overwrite_no')
        for name in ('a', 'b', 'result'):
            (src / name).write_bytes(name.encode())
        snaps = self.run(home, marks(('a', 'b')) + 'M-h RET ' + enter('result') + 'n ')
        require(snaps[-1].data['depth'] == 1, 'join: overwrite refusal ends the operation')
        require((src / 'result').read_bytes() == b'result', 'join: overwrite n preserves target')

        for mode in ('none', 'one', 'directory'):
            home, src = self.case('join_reject_' + mode)
            (src / 'a').write_bytes(b'A')
            (src / 'b').write_bytes(b'B')
            (src / 'directory').mkdir()
            selected = {'none': (), 'one': ('a',), 'directory': ('a', 'directory')}[mode]
            before = sorted(p.name for p in src.iterdir())
            snaps = self.run(home, marks(selected) + 'M-h ')
            require(any(word in rendered(snaps[-1]) for word in ('マーク', '2', '２', 'ディレクトリ', '対象')),
                    'join: invalid selection explains rejection')
            require(sorted(p.name for p in src.iterdir()) == before, 'join: invalid selection makes no output')

        home, src = self.case('join_scroll')
        names = [f'part{i:02d}' for i in range(35)]
        for i, name in enumerate(names):
            (src / name).write_bytes(bytes([i]))
        snaps = self.run(home, marks(names) + 'M-h SNAP # top\n' + 'DOWN ' * 34 +
                         'SNAP # bottom\nu SNAP # moved\nRET ' + enter('result'))
        for snap in snaps[:3]:
            self.box(snap, 'scroll order')
        require(names[-1] in rendered(snaps[1]), 'order scroll reveals last selected name')
        require(names[0] not in rendered(snaps[1]), 'order scroll moves viewport')
        expected = bytes(range(33)) + bytes((34, 33))
        require((src / 'result').read_bytes() == expected, 'join: reorder while scrolled')



    def boundaries(self):
        for value in ('0K', '1024M', '1G', '1.5M', '1', '999999999999999999999M'):
            home, src = self.case('bad_size_' + value)
            (src / 'source').write_bytes(b'x' * 2500)
            snaps = self.run(home, select('source') + 'M-n ' + enter(value))
            require('値が正しくありません' in rendered(snaps[-1]), 'invalid size ' + value)
            require(snaps[-1].data['depth'] == 1 and len(list(src.iterdir())) == 1,
                    'invalid size leaves filer and source intact')

        home, src = self.case('four_digits')
        data = bytes(range(256)) * 4001
        (src / 'source').write_bytes(data)
        self.run(home, select('source') + 'M-n ' + enter('1K') + enter('part') + 'y n ')
        parts = sorted(src.glob('part.[0-9][0-9][0-9][0-9]'))
        require(len(parts) == 1001 and parts[0].name == 'part.0001' and parts[-1].name == 'part.1001',
                'over 1000 pieces use four digits')
        require(b''.join(p.read_bytes() for p in parts) == data, 'four-digit piece contents')

        home, src = self.case('live_split_cancel')
        data = b'x' * (8 * 1024 * 1024)
        (src / 'source').write_bytes(data)
        snaps = self.run(home, select('source') + 'M-n ' + enter('4M') + enter('part') +
                         'y WAIT:0 WAIT:0 SNAP # live\nESC ', settle=False)
        require(snaps[0].data['depth'] > 1 and '分割中' in rendered(snaps[0]), 'split exposes tick progress')
        require(snaps[-1].data['depth'] == 1 and [p.name for p in src.iterdir()] == ['source'],
                'cancel removes the unfinished piece')
        require((src / 'source').read_bytes() == data, 'cancel preserves source bytes')

        for alias in (False, True):
            home, src = self.case('join_source_alias_' + str(alias))
            (src / 'a').write_bytes(b'A')
            (src / 'b').write_bytes(b'B')
            output = 'a'
            if alias:
                os.link(src / 'a', src / 'alias')
                output = 'alias'
            snaps = self.run(home, marks(('a', 'b')) + 'M-h RET ' + enter(output))
            require('結合元と同じ名前にはできません' in rendered(snaps[-1]), 'reject same input inode')
            require((src / 'a').read_bytes() == b'A' and (src / 'b').read_bytes() == b'B',
                    'same-input rejection preserves all sources')

        home, src = self.case('join_empty')
        (src / 'a').write_bytes(b'')
        (src / 'b').write_bytes(b'')
        self.run(home, marks(('a', 'b')) + 'M-h RET ' + enter('result'))
        require((src / 'result').read_bytes() == b'', 'join zero-length inputs')

        for split in (False, True):
            home, src = self.case('write_failure_' + str(split))
            (src / 'a').write_bytes(b'A' * (4 * 1024 * 1024))
            if split:
                keys = select('a') + 'M-n ' + enter('3M') + enter('part') + 'y '
            else:
                (src / 'b').write_bytes(b'B')
                keys = marks(('a', 'b')) + 'M-h RET ' + enter('result')
            # Limit only the app child: each screenshot fits below 2 MB, while
            # the output file reaches EFBIG midway through a bounded tick.
            snaps = self.run(home, keys, output_limit=2 * 1024 * 1024)
            require('書き出しに失敗しました' in rendered(snaps[-1]), 'write failure explained')
            require(snaps[-1].data['depth'] == 1, 'write failure returns to filer')
            require(not (src / ('part.001' if split else 'result')).exists(),
                    'write failure removes partial output')
            require((src / 'a').read_bytes() == b'A' * (4 * 1024 * 1024), 'failed write preserves input')

        home, src = self.case('overwrite_yes')
        data = b'A' * 2500
        (src / 'source').write_bytes(data)
        (src / 'part.001').write_bytes(b'old part')
        (src / 'part.join.sh').write_bytes(b'old script')
        self.run(home, select('source') + 'M-n ' + enter('1K') + enter('part') + 'y y y y ')
        require((src / 'part.001').read_bytes() == data[:1024], 'confirmed split overwrite')
        require((src / 'part.join.sh').stat().st_mode & 0o111, 'generated script executable')
        subprocess.run(['/bin/sh', str(src / 'part.join.sh')], cwd=src, env=self.env, check=True)
        require((src / 'part').read_bytes() == data, 'confirmed script overwrite')
        (src / 'result').write_bytes(b'old result')
        self.run(home, marks(('part.001', 'part.002', 'part.003')) + 'M-h RET ' + enter('result') + 'y ')
        require((src / 'result').read_bytes() == data, 'confirmed join overwrite')

        home, src = self.case('numbered_initial_and_order_cancel')
        for name in ('part.001', 'part.1000'):
            (src / name).write_bytes(b'x')
        snaps = self.run(home, marks(('part.001', 'part.1000')) + 'M-h RET SNAP # initial\nESC ')
        require(snaps[0].data['widget'] == 3 and input_text(snaps[0]) == 'part',
                'mixed suffix widths recognised')
        snaps = self.run(home, marks(('part.001', 'part.1000')) + 'M-h ESC ')
        require(snaps[-1].data['depth'] == 1 and len(list(src.iterdir())) == 2, 'order cancellation pops both screens')

        home, src = self.case('mixed_pdf')
        (src / 'a.pdf').write_bytes(b'%PDF-1.4')
        (src / 'b').write_bytes(b'B')
        snaps = self.run(home, marks(('a.pdf', 'b')) + 'M-h SNAP # mixed\nn ')
        require('PDF とそれ以外が混ざっています' in rendered(snaps[0]), 'mixed PDF warning')
        require(snaps[-1].data['depth'] == 1 and len(list(src.iterdir())) == 2, 'mixed PDF declined')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--size', default='80x25', choices=('80x25', '85x25'))
    args = parser.parse_args()
    build = args.build.resolve()
    require(build.is_relative_to(ROOT / 'build'), 'build and fixtures must be under build/')
    (build / 'tmp').mkdir(parents=True, exist_ok=True)
    subprocess.run(['make', '-C', str(ROOT), 'all', 'CFLAGS=-Werror', 'BUILD=' + str(build)],
                   env=dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(build / 'tmp')), check=True)
    runner = Runner(build, args.size)
    runner.split()
    runner.join()
    runner.boundaries()
    print(f'Split/join checks passed ({args.size}): {runner.count} scripts, {runner.snapshots} snapshots')


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError, subprocess.CalledProcessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
