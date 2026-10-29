#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The filer sees changes made by other programs (another FDTM, the Finder,
LocalSend's helper, a terminal): every FILER_LOOK_SECONDS it looks at the
shown directories (stat and the count of names) and reads a changed one
again, the cursor and the marks kept; not while a box is open, but at the
first look after it closes.

Run python3 -B tests/check_watch.py [--build BUILD]. Everything stays in
BUILD/watch-check.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

from check import check_snapshot, require, text

ROOT = Path(__file__).resolve().parents[1]


def names(snap):
    return [text(snap, y, 2, 10).strip() for y in range(6, 23) if text(snap, y, 2, 10).strip()]


def marked(snap):
    return [text(snap, y, 2, 10).strip() for y in range(6, 23) if text(snap, y, 1, 2) == '*']


def cursor(snap):
    return [text(snap, y, 2, 10).strip() for y in range(6, 23) if snap['rows'][y][4][2] != 0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=ROOT / 'build')
    build = parser.parse_args().build.resolve()
    base = build / 'watch-check'
    shutil.rmtree(base, ignore_errors=True)
    files = base / 'files'
    files.mkdir(parents=True)
    for name in ('a', 'b', 'c'):
        (files / f'{name}.txt').write_text(name)
    (base / 'home').mkdir()
    env = dict(os.environ, HOME=str(base / 'home'), TZ='UTC', FDTP_LANGUAGE='ja', FDTP_HISTORY='',
               FDTP_RC_SYSTEM='', FDTP_RC_USER=str(base / 'fdtprc'), FDTP_POMERA_CHECK='', FDTP_POMERA_SEEN='')
    executable = build / ('fdtp-preview' if sys.platform == 'darwin' else 'fdtp')
    prefix = base / 'snap'
    child = subprocess.Popen([str(executable), '--headless', '--script', str(ROOT / 'tests/watch.keys'),
                              '--out', str(prefix), '--time', '1700000000', str(files)],
                             cwd=ROOT.parent, env=env, stdout=subprocess.DEVNULL)

    def wait_for(n):
        end = time.monotonic() + 20
        while not (base / f'snap-{n:03d}.cells').exists():
            require(time.monotonic() < end and child.poll() is None, f'snapshot {n} did not come')
            time.sleep(0.02)

    wait_for(1)                                   # before: then another program changes it
    (files / 'new.txt').write_text('new')
    (files / 'c.txt').unlink()
    wait_for(3)                                   # after: the help opens next, then again
    time.sleep(0.5)                               # (inside the REALWAIT with the box open)
    (files / 'b.txt').unlink()
    require(child.wait(timeout=60) == 0, 'the headless run failed')
    labels = ['before', 'same_time', 'after', 'boxed', 'closed_now', 'closed']
    s = {label: check_snapshot(prefix.parent / f'snap-{i + 1:03d}.cells') for i, label in enumerate(labels)}
    require(names(s['before']) == ['..', 'a', 'b', 'c'] and marked(s['before']) == ['a'] and cursor(s['before']) == ['b'],
            f'before: {names(s["before"])} {marked(s["before"])} {cursor(s["before"])}')
    require(names(s['same_time']) == names(s['before']), 'no look before FILER_LOOK_SECONDS have passed')
    require(names(s['after']) == ['..', 'a', 'b', 'new'], f'read again: {names(s["after"])}')
    require(marked(s['after']) == ['a'] and cursor(s['after']) == ['b'], 'the mark and the cursor are kept')
    require(s['boxed']['widget'] != 0, 'the help box is open')
    # The box covers the list; the byte count at the right of row 4 shows:
    # a, b and new are 5 bytes, without b 4.
    bytes_of = lambda snap: text(snap, 4, 64, 79).strip()
    require(bytes_of(s['boxed']) == '5', f'not read again while the box was open: {bytes_of(s["boxed"])}')
    for label in ('closed_now', 'closed'):
        require(names(s[label]) == ['..', 'a', 'new'] and marked(s[label]) == ['a'] and bytes_of(s[label]) == '4',
                f'{label}: read again at the first look after it closed: {names(s[label])}')
    print('watch: OK')


if __name__ == '__main__':
    main()
