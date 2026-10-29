#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Revision 1 §11: the filer reads a shown directory again when another
program changes it (filer_tick looks every FILER_LOOK_SECONDS). A file is
added while the filer waits, then the marked one is removed: the list
follows, the mark of what is still there stays. Real time (REALWAIT:) is
what the look sees, so this takes some seconds.

  check_reload.py [--build BUILD] [--executable EXE]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time

from check import check_snapshot, require, text

ROOT = Path(__file__).resolve().parents[1]


def names(snap):
    """The names on the list (FD shows the extension apart: "a     .txt");
    a mark (*) is left off."""
    out = []
    for y in range(6, 20):
        t = text(snap, y).split()
        if not t:
            continue
        name = t[0].lstrip('*')
        if len(t) > 1 and t[1].startswith('.') and t[0] != '..':
            name += t[1]
        out.append(name)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', default=str(ROOT / 'build'))
    ap.add_argument('--executable')
    args = ap.parse_args()
    build = Path(args.build).resolve()
    exe = Path(args.executable) if args.executable else build / ('fdtp-preview' if sys.platform == 'darwin' else 'fdtp')
    base = build / 'reload-check'
    shutil.rmtree(base, ignore_errors=True)
    d = base / 'files'
    d.mkdir(parents=True)
    for name in ('a.txt', 'c.txt'):
        (d / name).write_text(name)
    keys = base / 'reload.keys'
    # Fixture order: .., a.txt, c.txt. The cursor on c.txt, a.txt marked.
    keys.write_text('DOWN TAB DOWN SNAP # before\nREALWAIT:3.5 SNAP # added\nREALWAIT:3.5 SNAP # removed\n')
    env = dict(os.environ, FDTP_LANGUAGE='ja', FDTP_HISTORY='', HOME=str(base), TZ='UTC', TMPDIR=str(build / 'tmp'))
    (build / 'tmp').mkdir(exist_ok=True)

    def outside():
        time.sleep(1.2)
        (d / 'b.txt').write_text('b')       # another program adds a file
        time.sleep(3.5)
        (d / 'a.txt').unlink()              # and removes the marked one
    worker = threading.Thread(target=outside)
    worker.start()
    subprocess.run([str(exe), '--headless', '--script', str(keys), '--out', str(base / 'snap'),
                    '--time', '1700000000', str(d)], check=True, env=env, stdout=subprocess.DEVNULL)
    worker.join()
    snaps = [check_snapshot(f) for f in sorted(base.glob('snap-*.cells'))]
    require(len(snaps) == 3, f'{len(snaps)} snapshots')
    before, added, removed = (names(s) for s in snaps)
    require(before == ['..', 'a.txt', 'c.txt'], f'before: {before}')
    require(added == ['..', 'a.txt', 'b.txt', 'c.txt'], f'a file added outside shows: {added}')
    require(removed == ['..', 'b.txt', 'c.txt'], f'a file removed outside goes: {removed}')
    marked = [text(snaps[1], y) for y in range(6, 20) if text(snaps[1], y).lstrip().startswith('*a')]
    require(marked, f'the mark on a.txt stays after the reload: {[text(snaps[1], y) for y in range(6, 10)]}')
    print('reload: ok')


if __name__ == '__main__':
    main()
