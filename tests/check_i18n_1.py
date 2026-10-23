#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Group 1 English UI acceptance: python3 -B tests/check_i18n_1.py --build build-1.

ASCII fixture names/content separate UI text from user data. Snapshot both
help columns and every scrolled page at narrow and wide screen sizes.
"""
import argparse
import importlib.util
from pathlib import Path
import re
import sys

from check_wide import Runner, require
from check_help import check_box, column_starts
from check_frames import check_screen

JAPANESE = re.compile(r'[\u3040-\u30ff\u3400-\u9fff]')


def help_labels():
    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location('group1_gen_i18n', root / 'tools/gen_i18n.py')
    gen = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = gen
    spec.loader.exec_module(gen)
    translations, errors = gen.read_translations(root)
    require(not errors, str(errors))
    source = (root / 'src/commands.c').read_text()
    body = source.split('help_items[]={', 1)[1].split('};', 1)[0]
    return [translations.get(gen.c_string_bytes(t.value).decode(),
                             gen.c_string_bytes(t.value).decode())
            for t in gen.tokens(body) if t.kind == 'string' and t.value]


def english(s, label):
    for y in range(s.nrows):
        require(not JAPANESE.search(s.text(y)), f'{label}: Japanese in row {y}: {s.text(y)!r}')
    for b in s.boxes():
        x, y, w, h = b[:4]
        require(0 <= x < x + w <= s.cols and 0 <= y < y + h <= s.nrows,
                f'{label}: box outside screen: {b}')
        if b[5] == 3:
            check_box(s, b, label)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    args = p.parse_args()
    build = args.build.resolve()
    (build / 'tmp').mkdir(parents=True, exist_ok=True)
    r = Runner(build, True)
    r.base = build / 'i18n-1-check'
    d = r.base / 'files'
    d.mkdir(parents=True, exist_ok=True)
    (d / 'sample.txt').write_text('ASCII content\n')
    r.env.update(FDTP_LANGUAGE='en', FDTP_RC_USER=str(r.base / 'none.rc'))
    for size in ('80x25', '85x25', '160x45'):
        cases = {
            'filer': ('SNAP', 'filer'),
            'help': ('h SNAP ' + 'PGDN SNAP ' * 12 + 'HOME SNAP END SNAP', 'box'),
            'sysinfo': ('M-i SNAP', 'full'),
            'settings': ('z SNAP ' + 'PGDN SNAP ' * 12 + 'END SNAP', 'full'),
            'keys': ('z RIGHT SNAP PGDN SNAP END SNAP', 'full'),
            'launcher': ('z RIGHT RIGHT SNAP END SNAP', 'full'),
            'input': ('l SNAP', 'box'),
            'quit': ('q SNAP', 'box'),
            'sort': ('s SNAP', 'box'),
            'error': ('l "/nonexistent-fdtp-i18n-1" RET SNAP', 'box'),
        }
        for name, (keys, kind) in cases.items():
            snaps = r.run(f'{name}-{size}', keys, d, ['--size', size])
            require(snaps, f'{name}: snapshots generated')
            for s in snaps:
                label = s.path.stem
                english(s, label)
                check_screen(s, kind, label)
                if name == 'help':
                    starts = column_starts(s, s.boxes()[-1])
                    # Each help column has a blank cell before its divider.
                    b = s.boxes()[-1]
                    for start in starts[1:]:
                        for y in range(b[1] + 1, b[1] + b[3] - 1):
                            require(s.cell(start - 3, y)[0] == 32,
                                    f'{label}: help text crosses column divider')
            if name == 'help':
                shown = '\n'.join(s.text(y) for s in snaps for y in range(s.nrows))
                for label in help_labels():
                    require(label in shown, f'{size}: help label clipped or missing: {label!r}')
    print(f'i18n group 1: {r.count} English snapshots passed')


if __name__ == '__main__':
    main()
