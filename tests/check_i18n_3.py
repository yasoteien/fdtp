#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""English novel/editor and viewer UI: python3 -B tests/check_i18n_3.py --build build-3.

Uses the real headless filer entry points (e/v/b), not a mock editor. All
fixtures, rc/state/autosave files and cell/PPM snapshots stay in BUILD.
Japanese manuscript/body text is deliberately present and excluded only in
the body; overlaid widgets are always checked, including their body rows.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from check import check_snapshot, popup_text, require, text

ROOT = Path(__file__).resolve().parents[1]
JAPANESE = re.compile(r'[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff\uff66-\uff9f]')


def english_and_frame(snap, label):
    """Check UI cells, all visible widget cells, border gaps and wide glyphs."""
    boxes = snap['boxes']
    require(boxes and boxes[0][:4] == [0, 0, 80, 23], label + ': viewer frame')
    require(snap['depth'] == 2, label + ': editor/viewer remains open')
    # The text viewer's body and hex character column contain user data;
    # the novel editor draws manuscript glyphs directly to the canvas.
    ui = {(x, y) for y in (0, 22, 23, 24) for x in range(80)}
    for i, (left, top, width, height, _rule, kind) in enumerate(boxes):
        right, bottom = left + width - 1, top + height - 1
        require(0 <= left < right < 80 and 0 <= top < bottom < 25,
                f'{label}: box {i} fits screen')
        if kind & 2:
            require(right < 79 and bottom < 24, f'{label}: box {i} shadow fits screen')
            ui.update((x, y) for y in range(top, bottom + 1)
                      for x in range(left, right + 1))
        gaps = snap['gaps'][i]
        for y in range(top + 1, bottom):
            for x in (left, right):
                # A later floating box may cover an earlier frame edge.
                if any(b[0] <= x < b[0] + b[2] and b[1] <= y < b[1] + b[3]
                       for b in boxes[i + 1:]):
                    continue
                require(snap['rows'][y][x][0] == 32,
                        f'{label}: text crossed box {i} side at {x},{y}')
        for y, start, end in ((top, gaps[0], gaps[1]), (bottom, gaps[2], gaps[3])):
            require((start == end == 0) or left < start < end <= right,
                    f'{label}: box {i} title/status gap fits border')
            for x in range(left, right + 1):
                if start <= x < end or any(
                        b[0] <= x < b[0] + b[2] and b[1] <= y < b[1] + b[3]
                        for b in boxes[i + 1:]):
                    continue
                require(snap['rows'][y][x][0] == 32,
                        f'{label}: text outside box {i} border gap at {x},{y}')
    for x, y in sorted(ui):
        ch = chr(snap['rows'][y][x][0])
        require(not JAPANESE.search(ch), f'{label}: untranslated {ch!r} at {x},{y}')
    require(text(snap, 24).strip(), label + ': nonempty command band')
    require(text(snap, 22).strip(), label + ': nonempty status')


class Runner:
    def __init__(self, build):
        self.build = build.resolve()
        self.base = self.build / 'i18n-3-fixture'
        if self.base.exists():
            shutil.rmtree(self.base)
        self.base.mkdir(parents=True)
        (self.build / 'tmp').mkdir(exist_ok=True)
        self.exe = self.build / ('fdtp-preview' if sys.platform == 'darwin' else 'fdtp')
        require(self.exe.is_file(), f'build executable missing: {self.exe}')
        self.total = 0

    def run(self, name, entry, steps):
        work = self.base / name
        home = work / 'home'
        files = work / 'files'
        home.mkdir(parents=True)
        files.mkdir()
        (files / 'sample.txt').write_text('abc 日本語の本文\n\n「原稿です。」\nabc\n')
        (home / '.fdtprc').write_text('LANGUAGE=en\n')
        script = work / 'case.keys'
        source = 'C-s "sample.txt" ESC ' + entry + '\n'
        source += ''.join(keys + ' SNAP\n' for _label, keys, _widget, _needle in steps)
        script.write_text(source)
        env = dict(os.environ)
        for key in list(env):
            if key.startswith('FDTP_'):
                env.pop(key)
        env.update(HOME=str(home), TMPDIR=str(self.build / 'tmp'), LANG='C',
                   FDTP_LANGUAGE='en', FDTP_RC_SYSTEM='',
                   FDTP_RC_USER=str(home / '.fdtprc'),
                   FDTP_NOVEL_STATE=str(work / 'novel-state'),
                   FDTP_NOVEL_AUTOSAVE=str(work / 'autosave'),
                   FDTP_WORD_TEMPLATE=str(work / 'missing-template.docx'))
        prefix = work / 'snapshot'
        subprocess.run([str(self.exe), '--headless', '--script', str(script),
                        '--out', str(prefix), '--time', '1700000000', str(files)],
                       env=env, check=True, stdout=subprocess.DEVNULL, timeout=90)
        paths = sorted(work.glob('snapshot-*.cells'))
        require(len(paths) == len(steps), name + ': snapshot count')
        shots = []
        for path, (label, _keys, widget, needle) in zip(paths, steps):
            snap = check_snapshot(path)
            label = name + '/' + label
            english_and_frame(snap, label)
            if widget is not None:
                require(snap['widget'] == widget, f'{label}: expected widget {widget}, got {snap["widget"]}')
            if needle:
                visible = '\n'.join(text(snap, y) for y in range(25))
                require(needle.lower() in visible.lower(), f'{label}: missing complete text {needle!r}')
            shots.append(snap)
        self.total += len(shots)
        print(f'  {name}: {len(shots)} English snapshots')
        return shots


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, default=ROOT / 'build-3')
    args = ap.parse_args()
    t = Runner(args.build)
    t.run('novel', 'e', [
        ('horizontal', '', 0, 'Horizontal'),
        ('vertical', 'C-t', 0, 'Vertical'),
        ('numbers', 'C-l', 0, None),
        ('help-top', 'F1', 7, 'Ctrl'),
        ('help-page', 'PGDN', 7, None),
        ('help-bottom', 'END', 7, 'F1'),
        ('search', 'ESC C-f', 0, 'Find'),
        ('search-miss', '"no-such-content" RET', 1, 'Not found'),
        ('replace-from', 'C-h', 0, None),
        ('replace-to', '"abc" RET', 0, None),
        ('replace-confirm', '"XYZ" RET', 0, None),
        ('goto', 'ESC C-g', 0, None),
        ('goto-last', '"9999" RET', 0, None),
        ('modified', '"X"', 0, 'Modified'),
        ('save-as', 'C-S-s', 3, None),
        ('close-confirm', 'ESC ESC', 0, 'Save'),
        ('close-cancel', 'ESC', 0, None),
        ('saved', 'C-s', None, None),
    ])
    s = t.run('novel-convert', 'e', [
        ('menu-format', 'F2', 4, None),
        ('confirm-format', 'RET', 2, None),
        ('format-results', 'y', None, None),
        ('menu-manuscript', 'ESC F2 DOWN', 4, None),
        ('confirm-manuscript', 'RET', 2, None),
        ('manuscript-results', 'y', None, None),
        # "No changes needed" is a status-line notice: it does not take Esc.
        ('menu-web', 'F2 DOWN DOWN', 4, None),
        ('confirm-web', 'RET', 2, None),
        ('web-results', 'y', None, None),
        ('menu-word', 'ESC F2 DOWN DOWN DOWN', 4, None),
        # No template anywhere: the built-in one (revision 1 4.3).
        ('word-confirm', 'RET', 2, None),
        ('word-default-template', 'y', None, 'with the default template'),
    ])
    # Entire conversion labels must be visible, not merely an English prefix.
    # Derive the agreed translation from the catalog so wording can evolve.
    translations = {}
    for catalog in (ROOT / 'i18n/en').glob('*.tsv'):
        for line in catalog.read_text().splitlines():
            if line and not line.startswith('#'):
                jp, en = line.split('\t', 1)
                translations[jp] = en
    for jp in ('出版社の書式に整形', '原稿形式へ（空行を除く）',
               'Web 形式へ（空行を入れる）', 'Word 縦書きで書き出す'):
        require(jp in translations, 'conversion translation missing: ' + jp)
        require(translations[jp] in popup_text(s[0]), 'conversion label clipped: ' + translations[jp])
    t.run('text', 'v', [
        ('initial', '', 0, 'UTF-8'),
        ('encoding', 'c', 4, 'UTF-16BE'),
        ('encoding-sjis', 'RIGHT RET', 0, 'Shift_JIS'),
        ('encoding-auto', 'c LEFT RET', 0, None),
        ('search', '/', 3, None),
        ('search-miss', '"no-such-content" RET', None, None),
        ('numbers', '"#"', None, None),
    ])
    t.run('hex', 'b', [
        ('initial', '', 0, None),
        ('swap-refused', 's', None, None),
        ('word', 'w', None, None),
        ('swapped', 's', None, None),
        ('dword', 'w', None, None),
        ('encoding', 'c', 4, 'UTF-16BE'),
        ('encoding-return', 'ESC', 0, None),
        ('goto', 'g', 3, None),
        ('goto-invalid', '"FFFFFF" RET', None, None),
        ('search', '/', 3, None),
        ('search-invalid', '"123" RET', None, None),
        ('modified', '"41"', None, 'Modified'),
        ('save-confirm', 'C-s', 2, None),
        ('save-cancel', 'n', 0, 'Modified'),
        ('saved', 'C-s y', None, None),
        ('discard-confirm', '"42" q', 2, None),
        ('discard-cancel', 'n', 0, 'Modified'),
    ])
    print(f'i18n batch 3: {t.total} snapshots passed (English UI, frame bounds, PPM margins)')


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError, subprocess.SubprocessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
