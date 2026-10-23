#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""I18n foundation acceptance: python3 -B tests/check_i18n.py --build build.

Language selection, the first-run picker and a few translated messages.
All rc files and FDTP_SNAPSHOT_PREFIX output stay in BUILD/i18n-fixture.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

from check import check_snapshot, popup_text, require, text

ROOT = Path(__file__).resolve().parents[1]


class Acceptance:
    def __init__(self, build):
        self.build = build.resolve()
        self.base = self.build / 'i18n-fixture'
        if self.base.exists():
            shutil.rmtree(self.base)
        self.base.mkdir(parents=True)
        self.executable = self.build / ('fdtp-preview' if sys.platform == 'darwin' else 'fdtp')
        self.cases = self.snapshots = 0

    def run(self, name, keys, rc=None, system=None, env=None, home=None):
        home = home or self.base / name
        home.mkdir(exist_ok=True)
        prefix = home / name
        e = dict(os.environ)
        for key in ('FDTP_LANGUAGE', 'FDTP_TEST_KEYS', 'FDTP_FIRST_RUN_TEST', 'LC_ALL', 'LC_MESSAGES', 'LANG'):
            e.pop(key, None)
        e.update(HOME=str(home), TMPDIR=str(self.build / 'tmp'),
                 FDTP_RC_SYSTEM='', FDTP_SNAPSHOT_PREFIX=str(prefix), LANG='C')
        e.pop('FDTP_RC_USER', None)
        if rc is not None:
            (home / '.fdtprc').write_text(rc)
        if system is not None:
            path = home / 'system.rc'
            path.write_text(system)
            e['FDTP_RC_SYSTEM'] = str(path)
        e.update(env or {})
        subprocess.run([str(self.executable), '--headless', '--keys', keys,
                        '--time', '1700000000', str(home)], env=e,
                       check=True, stdout=subprocess.DEVNULL)
        shots = [check_snapshot(p) for p in sorted(home.glob(name + '-*.cells'))]
        self.cases += 1
        self.snapshots += len(shots)
        return shots, home


def settings_title(snap):
    return text(snap, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, default=ROOT / 'build')
    args = ap.parse_args()
    t = Acceptance(args.build)
    for name, rc, system, env, english in [
        ('ja', None, None, {'FDTP_LANGUAGE': 'ja'}, False),
        ('en', None, None, {'FDTP_LANGUAGE': 'en'}, True),
        ('user-ja', 'LANGUAGE=ja\n', 'LANGUAGE=en\n', {}, False),
        ('user-en', 'LANGUAGE=en\n', 'LANGUAGE=ja\n', {}, True),
        ('env-priority', 'LANGUAGE=ja\n', None, {'FDTP_LANGUAGE': 'en'}, True),
        ('auto-ja', None, None, {'LANG': 'ja_JP.UTF-8'}, False),
        ('auto-en', None, None, {'LANG': 'en_US.UTF-8'}, True),
        ('auto-empty', None, None, {'LANG': ''}, True),
        ('lc-all', None, None, {'LANG': 'ja_JP', 'LC_MESSAGES': 'ja', 'LC_ALL': 'C'}, True),
        ('lc-messages', None, None, {'LANG': 'C', 'LC_ALL': '', 'LC_MESSAGES': 'ja_JP'}, False),
        ('explicit', 'LANGUAGE=ja\n', None, {'LC_ALL': 'C'}, False),
        ('invalid-env', 'LANGUAGE=ja\n', None, {'FDTP_LANGUAGE': 'invalid'}, False),
    ]:
        s, _ = t.run(name, 'z SNAP', rc, system, env)
        expected = 'FDTP settings' if english else 'FDTP の設定'
        require(expected in settings_title(s[0]), f'{name}: language selection')
    # Three existing strings, plus the new LANGUAGE explanation, translated.
    s, _ = t.run('english-samples', 'ESC SNAP n z END UP UP UP UP SNAP RET SNAP',
                 env={'FDTP_LANGUAGE': 'en'})
    require('Quit FDTP?' in popup_text(s[0]), 'English quit confirmation')
    require('FDTP settings' in settings_title(s[1]), 'English settings title')
    require('Language: auto' in text(s[1], 21), 'English LANGUAGE help')
    require('Modified' in settings_title(s[2]), 'English modified marker')
    # LANGUAGE sits immediately before SCREENSIZE; the last three settings
    # keep their existing END-relative navigation positions.
    s, h = t.run('customizer', 'z END UP UP UP UP SNAP RET SNAP RET SNAP RET SNAP ESC y SNAP',
                 env={'LANG': 'C'})
    require('auto' in ''.join(text(s[0], y) for y in range(5, 20)), 'auto setting visible')
    require('FDTP の設定' in settings_title(s[1]), 'auto -> ja redraw immediately')
    require('FDTP settings' in settings_title(s[2]), 'ja -> en redraw immediately')
    require('FDTP settings' in settings_title(s[3]), 'en -> auto resolves locale')
    require('LANGUAGE=auto' in (h / '.fdtprc').read_text(), 'customizer persists LANGUAGE')
    s, _ = t.run('override', 'z END UP UP UP UP RET SNAP', env={'FDTP_LANGUAGE': 'en'})
    require('FDTP settings' in settings_title(s[0]), 'environment remains highest priority after change')
    s, h = t.run('first-en', 'SNAP 2 SNAP z SNAP', env={'FDTP_FIRST_RUN_TEST': '1'})
    require('Language / 言語' in popup_text(s[0]), 'first-run bilingual title')
    require(any(b[5] == 3 for b in s[0]['boxes']), 'first-run frame and shadow')
    require((h / '.fdtprc').read_text() == '# FDTP language\nLANGUAGE=en\n', 'first-run exact two-line file')
    require('FDTP settings' in settings_title(s[2]), 'first-run selection applied immediately')
    s, _ = t.run('existing', 'z SNAP', env={'FDTP_FIRST_RUN_TEST': '1'}, home=h)
    require('FDTP settings' in settings_title(s[0]), 'existing rc suppresses picker and restores language')
    for name, keys in [('first-ja', '1'), ('first-down', 'UP DOWN RET'), ('first-up', 'DOWN UP RET')]:
        s, h = t.run(name, keys + ' z SNAP', env={'FDTP_FIRST_RUN_TEST': '1'})
        value = 'en' if name == 'first-down' else 'ja'
        require(f'LANGUAGE={value}' in (h / '.fdtprc').read_text(), name + ': selection saved')
    _, h = t.run('first-escape', 'ESC SNAP', env={'FDTP_FIRST_RUN_TEST': '1'})
    require(not (h / '.fdtprc').exists(), 'Esc creates no rc')
    s, _ = t.run('first-again', 'SNAP', home=h, env={'FDTP_FIRST_RUN_TEST': '1'})
    require('Language / 言語' in popup_text(s[0]), 'Esc asks again next launch')
    for name, env in [('test-keys', {'FDTP_TEST_KEYS': ''}), ('language-env', {'FDTP_LANGUAGE': 'ja'}),
                      ('headless', {})]:
        if name != 'headless':
            env['FDTP_FIRST_RUN_TEST'] = '1'
        s, h = t.run(name, 'SNAP', env=env)
        require('Language / 言語' not in popup_text(s[0]), name + ': picker suppressed')
        require(not (h / '.fdtprc').exists(), name + ': rc untouched')
    s, h = t.run('system-language', 'SNAP', system='LANGUAGE=en\n', env={'FDTP_FIRST_RUN_TEST': '1'})
    require('Language / 言語' not in popup_text(s[0]), 'LANGUAGE in /etc/fdtprc suppresses picker')
    require(not (h / '.fdtprc').exists(), 'system-language: rc untouched')
    s, _ = t.run('invalid-setting', 'z SNAP', rc='LANGUAGE=bogus\n', env={'LANG': 'ja'})
    require('FDTP の設定' in settings_title(s[0]), 'invalid LANGUAGE falls back to auto')
    s, h = t.run('first-write-error', '2 SNAP', env={
        'FDTP_FIRST_RUN_TEST': '1',
        'FDTP_RC_USER': str(t.base / 'missing-parent' / '.fdtprc')})
    # Nothing was saved, so the message follows the language in effect (LANG=C: English).
    require('Cannot create ~/.fdtprc' in popup_text(s[0]), 'first-run save failure is visible')
    require(not (h / '.fdtprc').exists(), 'write failure creates no user rc')
    s, h = t.run('first-write-error-ja', '1 SNAP', env={
        'FDTP_FIRST_RUN_TEST': '1', 'LANG': 'ja_JP.UTF-8',
        'FDTP_RC_USER': str(t.base / 'missing-parent' / '.fdtprc')})
    require('~/.fdtprc を作れません' in popup_text(s[0]), 'first-run save failure in Japanese')
    require(not (h / '.fdtprc').exists(), 'write failure creates no user rc (ja)')
    print(f'i18n: {t.cases} cases, {t.snapshots} snapshots passed')


if __name__ == '__main__':
    main()
