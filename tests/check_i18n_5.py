#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""English group 5 UI: python3 -B tests/check_i18n_5.py --build build-5.

Real launcher acceptance; cells and PPMs stay in BUILD/i18n-5. Japanese
file contents are excluded from the chrome checks. Includes F5 slideshow.
"""
import argparse
import base64
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from check import require, text, popup_text

ROOT = Path(__file__).resolve().parents[1]
JAPANESE = re.compile(r'[\u3040-\u30ff\u3400-\u9fff]')

# 32x32 baseline JPEG (top half blue, bottom yellow), made once by
# `tests/slide_harness make-jpeg`; embedded so the test needs only the
# standard library.
JPEG = base64.b64decode(
    '/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYFBgYGBwkIBgcJBwYGCAsI'
    'CQoKCgoKBggLDAsKDAkKCgr/2wBDAQICAgICAgUDAwUKBwYHCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoK'
    'CgoKCgoKCgoKCgoKCgr/wAARCAAgACADASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAA'
    'AgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6'
    'Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXG'
    'x8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREA'
    'AgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5'
    'OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPE'
    'xcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD8c6KKK/38PysKKKKAP7FKKKK/4Az+qAoo'
    'ooA//9k='
)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Suite:
    def __init__(self, build):
        self.build = build.resolve()
        self.base = self.build / 'i18n-5'
        if self.base.exists():
            shutil.rmtree(self.base)
        self.base.mkdir(parents=True)
        self.files = self.base / 'files'
        self.files.mkdir()
        self.home = self.base / 'home'
        self.home.mkdir()
        self.exe = self.build / ('fdtp-preview' if sys.platform == 'darwin' else 'fdtp')
        self.count = 0

    def run(self, name, filename, keys, expected, edit_rows=(), fullscreen=()):
        prefix = self.base / name
        env = dict(os.environ, HOME=str(self.home), FDTP_RC_USER='', FDTP_RC_SYSTEM='',
                   FDTP_LANGUAGE='en', LANG='C', TMPDIR=str(self.build / 'tmp'))
        script = f'C-s "{filename}" ESC v ' + keys
        subprocess.run([str(self.exe), '--headless', '--keys', script, '--out', str(prefix),
                        '--time', '1700000000', str(self.files)], env=env, check=True,
                       stdout=subprocess.DEVNULL, timeout=60)
        paths = sorted(self.base.glob(name + '-*.cells'))
        require(len(paths) == len(expected), f'{name}: snapshot count')
        for i, (path, wanted) in enumerate(zip(paths, expected)):
            snap = json.loads(path.read_text())
            require(len(snap['rows']) == 25 and all(len(r) == 80 for r in snap['rows']), str(path))
            for row in snap['rows']:
                for x, cell in enumerate(row):
                    if cell[3] == 1:
                        require(x < 79 and row[x + 1] == [*cell[:3], 2], f'{path}: split wide glyph')
            if i in fullscreen:
                require(not snap.get('boxes'), f'{path}: slideshow frame hidden')
                require(all(c[0] == 32 for r in snap['rows'] for c in r), f'{path}: slideshow chrome hidden')
            else:
                rows = [0, 22, 24] + ([] if i in edit_rows else [23])
                chrome = '\n'.join(text(snap, y) for y in rows) + popup_text(snap)
                if i in edit_rows:
                    # Row 23 is "label: data" or "Paragraph 1/2  data"; only the label is UI.
                    chrome += '\n' + re.split(r': |(?<=\d)  ', text(snap, 23), maxsplit=1)[0]
                require(not JAPANESE.search(chrome), f'{path}: untranslated chrome: {chrome}')
                require(wanted in chrome, f'{path}: expected {wanted!r}: {chrome}')
                require(snap.get('depth', 0) == 2, f'{path}: viewer open')
                for col, row, w, h, rule, kind in snap.get('boxes', []):
                    require(0 <= col and col + w <= 80 and 0 <= row and row + h <= 25,
                            f'{path}: box outside canvas')
                    if kind & 2:
                        for y in range(row + 1, row + h - 1):
                            require(snap['rows'][y][col][0] == 32 and snap['rows'][y][col + w - 1][0] == 32,
                                    f'{path}: text on popup border')
                if snap.get('widget', 0) == 0:
                    band = text(snap, 24)
                    require('Back' in band or 'Quit' in band or 'OK' in band,
                            f'{path}: clipped key band: {band}')
            require(path.with_suffix('.ppm').read_bytes().startswith(b'P6\n1024 600\n255\n'), f'{path}: image')
            self.count += 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, default=ROOT / 'build-5')
    t = Suite(ap.parse_args().build)
    office = load('office5', ROOT / 'tools/make_test_office.py')
    images = load('images5', ROOT / 'tools/make_test_images.py')
    (t.files / 'data.csv').write_text('name,value\nalpha,1\n')
    (t.files / 'data.tsv').write_text('name\tvalue\nalpha\t1\n')
    parts = [(n, d.replace('売上', 'Sales').replace('予定', 'Plan').replace('空', 'Empty')
              if n == 'xl/workbook.xml' else d) for n, d in office.xlsx_parts()]
    office.package(t.files / 'book.xlsx', parts)
    office.make_docx(t.files / 'doc.docx')
    office.make_pptx(t.files / 'deck.pptx', JPEG)
    for ext, writer in [('png', images.png), ('bmp', images.bmp), ('gif', images.gif),
                        ('mag', images.mag), ('pi', images.pi)]:
        writer(t.files / ('picture.' + ext))
    t.run('csv', 'data.csv', 'SNAP C-g SNAP ESC "changed" RET SNAP C-s SNAP q',
          ['UTF-8', 'Go to cell', 'Modified', 'Saved'])
    t.run('tsv', 'data.tsv', 'SNAP', ['UTF-8'])
    # Row 23 shows the current cell's contents (Japanese data in this book).
    t.run('xlsx', 'book.xlsx', 'SNAP C-g SNAP ESC', ['1/3', 'Go to cell'], edit_rows=(0, 1))
    t.run('doc', 'doc.docx', 'SNAP e SNAP ESC C-k SNAP n',
          ['Line', 'Paragraph', 'Delete this paragraph'], edit_rows=(1,))
    t.run('slide', 'deck.pptx', 'SNAP g SNAP ESC e SNAP RET SNAP ESC ESC F5 SNAP RIGHT SNAP ESC SNAP',
          ['Slide 1/6', 'Slide number', 'Paragraph', 'OK', '', '', 'Slide 2/6'],
          edit_rows=(2, 3), fullscreen=(4, 5))
    for ext in ('png', 'bmp', 'gif', 'mag', 'pi'):
        t.run('image-' + ext, 'picture.' + ext, 'SNAP i SNAP', ['Zoom in', '16'])
    print(f'i18n group 5: {t.count} English snapshots passed (including F5 slideshow)')


if __name__ == '__main__':
    main()
