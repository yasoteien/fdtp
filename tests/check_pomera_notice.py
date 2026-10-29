#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Revision 1 §6: when the Pomera's boot check wrote state=damaged, FDTP shows
an information box at start, once per boot_id (FDTP_POMERA_CHECK and
FDTP_POMERA_SEEN point at files under BUILD/pomera-notice-check)."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

from check import check_snapshot, popup_rows, require

ROOT = Path(__file__).resolve().parents[1]


def run(build, base, check, seen, lang='ja'):
    exe = build / ('fdtp-preview' if sys.platform == 'darwin' else 'fdtp')
    prefix = base / 'snap'
    for old in base.glob('snap-*'):
        old.unlink()
    env = dict(os.environ, FDTP_LANGUAGE=lang, TMPDIR=str(build / 'tmp'), TZ='UTC', HOME=str(base / 'home'),
               FDTP_HISTORY='', FDTP_POMERA_CHECK=str(check), FDTP_POMERA_SEEN=str(seen))
    subprocess.run([str(exe), '--headless', '--script', str(ROOT / 'tests/pomera_notice.keys'), '--out', str(prefix),
                    '--time', '1700000000', str(base / 'files')], check=True, env=env, stdout=subprocess.DEVNULL)
    files = sorted(base.glob('snap-*.cells'))
    require(len(files) == 2, f'{len(files)} snapshots')
    return [check_snapshot(f) for f in files]


def body(snap):
    return ''.join(t for _, t in popup_rows(snap)).replace(' ', '')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', default=str(ROOT / 'build'))
    build = Path(ap.parse_args().build).resolve()
    base = build / 'pomera-notice-check'
    shutil.rmtree(base, ignore_errors=True)
    (base / 'files').mkdir(parents=True)
    (base / 'home').mkdir()
    (base / 'files' / 'a.txt').write_text('a\n')
    check, seen = base / 'check', base / 'seen'
    damaged = 'state=damaged\nmount=ro\nboot_id=b1\ntime=2026-10-29T13:45:53+09:00\nfsck_output=/var/lib/pomera-internal/fsck.txt\n'

    check.write_text(damaged)
    start, closed = run(build, base, check, seen)
    text = body(start)
    require('破損が見つかりました' in text and '読み取り専用' in text and 'fsck.txt' in text and 'fsck.fat-r' in text,
            f'damaged: the box says it: {text}')
    require(not popup_rows(closed), 'ESC closes it')
    require(seen.read_text().strip() == 'b1', 'the boot_id is kept')

    start, _ = run(build, base, check, seen)
    require(not popup_rows(start), 'the same boot: shown once')

    check.write_text(damaged.replace('b1', 'b2').replace('mount=ro', 'mount=none'))
    start, _ = run(build, base, check, seen, 'en')
    text = body(start)
    require('founddamage' in text and 'Notmounted' in text, f'next boot, English: {text}')

    for state in ('clean', 'known_only'):
        seen.unlink(missing_ok=True)
        check.write_text(damaged.replace('damaged', state))
        start, _ = run(build, base, check, seen)
        require(not popup_rows(start) and not seen.exists(), f'{state}: no box')

    check.unlink()
    start, _ = run(build, base, check, seen)
    require(not popup_rows(start), 'no check file: no box')
    print('pomera notice: ok')


if __name__ == '__main__':
    main()
