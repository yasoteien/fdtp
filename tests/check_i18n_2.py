#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Group 2 English UI: python3 -B tests/check_i18n_2.py --build build-2.

Uses only local headless execution. MuPDF-only cases are explicitly skipped
when the executable provides the platform stub. No SSH or deployment.
"""
import argparse
from pathlib import Path
import re

from check import require
from check_splitjoin import Runner, select, enter, marks, rendered
from check_pdftools import simple_pdf

JP = re.compile(r'[\u3040-\u30ff\u3400-\u9fff]')


def english(runner, snapshots, expected):
    require(len(snapshots) >= len(expected), 'all requested snapshots captured')
    for snap, phrase in zip(snapshots, expected):
        require(phrase in rendered(snap), f'expected English text: {phrase}')
        # ASCII fixture names and PDF text make the entire screen checkable.
        require(not JP.search(rendered(snap)), f'Japanese UI remains: {snap.path}')
        runner.box(snap, phrase) if snap.boxes() else None
        for x, y, w, h, _, _ in snap.boxes():
            require(x >= 0 and y >= 0 and x + w <= snap.cols and y + h <= snap.nrows,
                    f'box within screen: {snap.path}')
        # Runner.box also samples the pixels of both sides of floating frames.


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, required=True)
    args = ap.parse_args()
    for size in ('80x25', '85x25', '120x40'):
        r = Runner(args.build.resolve(), size)
        r.env['FDTP_LANGUAGE'] = 'en'
        home, src = r.case('english_split')
        data = bytes(range(256)) * 10
        (src / 'source').write_bytes(data)
        snaps = r.run(home, select('source') + 'M-n SNAP\n' + enter('1K') +
                      'SNAP\n' + enter('part') + 'SNAP\ny SNAP\nn ')
        english(r, snaps, ['Part size', 'Split filename prefix', 'Create 3 parts',
                          'Create a shell script', 'Parts created: 3'])
        require(b''.join(p.read_bytes() for p in sorted(src.glob('part.[0-9]*'))) == data,
                'English split preserves bytes')
        home, src = r.case('english_join')
        (src / 'a').write_bytes(b'A')
        (src / 'b').write_bytes(b'B')
        snaps = r.run(home, marks(('a', 'b')) + 'M-h SNAP\nd SNAP\nRET SNAP\n' + enter('result'))
        english(r, snaps, ['Files to join and their order', 'u:Up d:Down Enter:OK Esc:Cancel',
                          'Joined filename', 'Joined: 2; bytes: 2'])
        require((src / 'result').read_bytes() == b'BA', 'English order/join preserves selected order')
        home, src = r.case('english_mixed')
        (src / 'a.pdf').write_bytes(simple_pdf([(210, 300)]))
        (src / 'b').write_bytes(b'B')
        english(r, r.run(home, marks(('a.pdf', 'b')) + 'M-h SNAP\nn '),
                ['Mixed PDF and other files'])
        home, src = r.case('english_pdf')
        (src / 'sample.pdf').write_bytes(simple_pdf([(210, 300), (220, 300)]))
        snaps = r.run(home, select('sample.pdf') + 'M-p SNAP\n')
        if 'PDF tools unavailable' in rendered(snaps[0]):
            english(r, snaps, ['PDF tools unavailable on this system'])
            print(f'  SKIP MuPDF viewer/menu/job on {size}: platform stub')
        else:
            english(r, snaps, ['PDF menu'])
            # Exercise menu labels, page-range and angle choices, and viewer help.
            english(r, r.run(home, select('sample.pdf') + 'M-p r SNAP\n'), ['Rotation'])
            snaps = r.run(home, select('sample.pdf') + 'RET SNAP\nh SNAP\n')
            english(r, snaps, ['Page 1/2', 'PDF keys'])
        print(f'English group 2 checks passed: {size}, {r.snapshots} snapshots')


if __name__ == '__main__':
    main()
