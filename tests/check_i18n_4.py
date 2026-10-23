#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Group 4 English UI: python3 -B tests/check_i18n_4.py --build build-4.

ASCII fixtures keep file data separate from UI. Check English labels and frame
pixels at the minimum, Pomera, and wide screen sizes. No external host access.
"""
import argparse
from pathlib import Path
import re
import shutil
import zipfile
from check_wide import Runner, require
from check_frames import check_screen

ROOT = Path(__file__).resolve().parents[1]
JP = re.compile(r'[\u3040-\u30ff\u3400-\u9fff]')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, default=ROOT / 'build-4')
    args = ap.parse_args()
    runner = Runner(args.build.resolve(), False)
    runner.base = runner.build / 'i18n-4-fixture'
    shutil.rmtree(runner.base, ignore_errors=True)
    (runner.base / 'home').mkdir(parents=True)
    runner.env.update(FDTP_LANGUAGE='en', HOME=str(runner.base / 'home'),
                      FDTP_RC_USER=str(runner.base / 'no.rc'))
    files = runner.base / 'files'
    files.mkdir()
    (files / 'dir').mkdir()
    (files / 'dst').mkdir()
    (files / 'text.txt').write_text('ASCII content\n')
    (files / 'dst' / 'text.txt').write_text('existing\n')
    (files / 'link').symlink_to('text.txt')
    with zipfile.ZipFile(files / 'arc.zip', 'w') as z:
        z.writestr('inside.txt', 'ASCII archive content\n')
    (files / 'bad.zip').write_bytes(b'broken')
    select = 'C-s "text.txt" ESC '
    cases = [
        ('copy', select + 'c', 'box', 'Copy destination'),
        ('move', select + 'm', 'box', 'Destination'),
        ('rename', select + 'r', 'box', 'New name'),
        ('mkdir', 'k', 'box', 'Directory name'),
        ('delete', select + 'd', 'box', 'Delete files: 1?'),
        ('rmdir', 'C-s "dir" ESC C-d', 'box', 'Delete directory'),
        ('exec', select + 'x', 'box', 'Exec'),
        ('shell', 'S-F6', 'box', 'Shell'),
        ('attr', select + 'a', 'box', 'Attr'),
        ('attr-recursive', 'C-s "dir" ESC C-a', 'box', 'Attr'),
        ('info', select + 'i', 'box', 'File info'),
        ('link-info', 'C-s "link" ESC i', 'box', 'Symbolic link'),
        ('filesystem-input', 'S-F2', 'box', 'Path for filesystem info'),
        ('filesystem', 'S-F2 RET', 'box', 'Filesystem info'),
        ('tree', 't', 'full', 'Directory tree'),
        ('copy-tree', select + 'c F8', 'full', 'tree'),
        ('find-input', 'C-f', 'box', 'Directory pattern'),
        # The filer lists names in name/extension columns ("inside  .txt").
        ('archive', 'C-s "arc.zip" ESC v', 'filer', 'inside'),
        ('unpack', 'C-s "arc.zip" ESC u', 'box', 'Unpack'),
        ('unpack-tree', 'C-s "arc.zip" ESC u F8', 'full', 'tree'),
        ('pack', select + 'p', 'box', 'Pack'),
        ('bad-archive', 'C-s "bad.zip" ESC v', 'box', 'archive'),
        ('conflict', select + 'c C-u "dst" RET', 'box', 'Overwrite'),
    ]
    for size in ('80x25', '85x25', '160x45'):
        for name, keys, kind, expected in cases:
            label = name + '-' + size
            snaps = runner.run(label, keys + ' WAIT:0 WAIT:0 WAIT:0 WAIT:0 SNAP', files, args=('--size', size), expect=1)
            snap = snaps[0]
            rendered = '\n'.join(snap.text(y) for y in range(snap.nrows))
            require(not JP.search(rendered), f'{label}: untranslated Japanese: {rendered}')
            require(expected.lower() in rendered.lower(), f'{label}: missing {expected!r}: {rendered}')
            check_screen(snap, kind, label)
            for x, y, w, h, _, _ in snap.boxes():
                require(0 <= x and x+w <= snap.cols and 0 <= y and y+h <= snap.nrows,
                        f'{label}: box exceeds screen')
                for row in range(y+1, y+h-1):
                    # Border cells must not contain letters painted over the frame.
                    for col in (x, x+w-1):
                        code = snap.cell(col, row)[0]
                        require(not chr(code).isalnum(), f'{label}: text on border {col},{row}')
    print(f'i18n group 4: {len(cases)*3} English snapshots passed')


if __name__ == '__main__':
    main()
