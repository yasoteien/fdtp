#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Pixel comparison of two FDTP builds (WIDE_NOTES.md).

  pixel_compare.py run DIR      run every Mac check (make check and the
                                check_*.py but check_pdf) with $FDTP_PPM_COPY=DIR:
                                canvas.c writes a copy of every snapshot there
  pixel_compare.py compare A B  every snapshot in A must be in B, byte for byte

Run `run` once on the old commit and once on the new one (same work tree,
so the paths on the screen are the same), then `compare`. Snapshots that
show the time of day or files made during the run may differ for that
reason only; the output lists them for a look.
"""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CHECKS = ['check_archive', 'check_config', 'check_doc', 'check_fileops', 'check_imageview', 'check_keys',
          'check_lha', 'check_novel', 'check_novel_format', 'check_popup', 'check_sheet', 'check_slide',
          'check_viewers', 'check_wide']


def run(copy):
    copy = copy.resolve()
    copy.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, FDTP_PPM_COPY=str(copy))
    build = ROOT / 'build'
    failed = []
    make = ['make', '-C', str(ROOT), 'check', 'CFLAGS=-Werror']
    if sys.platform == 'darwin':
        # Disk usage and the times of files the checks make differ from run
        # to run: tools/fixenv.c makes them the same in both.
        lib = build / 'fixenv.dylib'
        build.mkdir(exist_ok=True)
        subprocess.run(['cc', '-dynamiclib', '-o', str(lib), str(ROOT / 'tools' / 'fixenv.c')], check=True)
        env['DYLD_INSERT_LIBRARIES'] = str(lib)
        make.append(f'FIXENV={lib}')
    steps = [make]
    steps += [[sys.executable, '-B', str(ROOT / 'tests' / f'{name}.py'), '--build', str(build)]
              for name in CHECKS if (ROOT / 'tests' / f'{name}.py').exists()]
    for step in steps:
        r = subprocess.run(step, cwd=ROOT.parent, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        name = Path(step[2]).stem if step[0] == sys.executable else 'make check'
        print(f'{name}: {"ok" if r.returncode == 0 else "FAILED"}')
        if r.returncode:
            failed.append(name)
            print(r.stderr[-2000:])
    print(f'{len(list(copy.iterdir()))} snapshots in {copy}')
    return 1 if failed else 0


def key(path):
    """The name from the build directory on, so that a copy of the old
    commit somewhere else (fdtp/build/base/<same name as the work tree>/)
    compares with the work tree."""
    name = path.name
    at = name.rfind('_fdtp_build_')
    return name[at:] if at >= 0 else name


def compare(a, b):
    old = {key(p): p for p in a.iterdir()}
    new = {key(p): p for p in b.iterdir()}
    missing = sorted(set(old) - set(new))
    differ = sorted(n for n in set(old) & set(new) if old[n].read_bytes() != new[n].read_bytes())
    print(f'{len(old)} old snapshots, {len(new)} new, {len(missing)} missing, {len(differ)} different')
    for n in missing:
        print('missing', n)
    for n in differ:
        print('differ ', n)
    return 1 if missing or differ else 0


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'run':
        sys.exit(run(Path(sys.argv[2])))
    if len(sys.argv) == 4 and sys.argv[1] == 'compare':
        sys.exit(compare(Path(sys.argv[2]), Path(sys.argv[3])))
    print(__doc__)
    sys.exit(2)
