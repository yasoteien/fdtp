#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""pomera/pomera_atok_mozc.py with the fake fdtp-dict (tests/dict_helper_fake.py):
the user's own Mozc dictionaries stay byte for byte, the ATOK ones are added
once (names off), switched, and removed; the profile directory follows Mozc."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'pomera' / 'pomera_atok_mozc.py'
FAKE = ROOT / 'tests' / 'dict_helper_fake.py'
sys.path.insert(0, str(ROOT / 'pomera'))
import pomera_atok_mozc as m  # noqa: E402


def require(cond, what):
    if not cond:
        raise SystemExit('FAIL: ' + what)


def run(home, *args, env=None):
    e = dict(os.environ, HOME=str(home), FDTP_DICT_BIN=str(FAKE), LANG='C')
    e.pop('XDG_CONFIG_HOME', None)
    e.update(env or {})
    return subprocess.run([sys.executable, str(SCRIPT), *args], env=e, capture_output=True, text=True)


def dictionaries(path):
    out = []
    for num, wt, value, raw in m.fields(path.read_bytes()):
        if num == 2:
            name, enabled, n = m.dictionary_info(value)
            out.append((name, enabled, n, raw))
    return out


def main():
    os.chmod(FAKE, 0o755)
    with tempfile.TemporaryDirectory(prefix='fdtp-atok-mozc-') as tmp:
        home = Path(tmp) / 'home'
        mozc = home / '.mozc'
        mozc.mkdir(parents=True)
        own = m.dictionary('私の辞書', [('ふぉお', '風王', 1)], True)
        old = m.field(1, 7) + own + m.field(10, 1)   # version, the user's own, storage_type
        db = mozc / 'user_dictionary.db'
        db.write_bytes(old)
        r = run(home, 'install')
        require(r.returncode == 0, 'install: ' + r.stdout + r.stderr)
        d = dictionaries(db)
        require(d[0][3] == own, 'own dictionary kept byte for byte')
        require(db.read_bytes().startswith(m.field(1, 7) + own + m.field(10, 1)), 'other fields kept')
        names = {n: (e, c) for n, e, c, _ in d}
        require(names == {'私の辞書': (True, 1), 'ATOK スポーツ辞書': (True, 2), 'ATOK アニメ辞書': (True, 1),
                          'ATOK エンタメ辞書': (True, 1), 'ATOK 人名辞書': (False, 3)}, f'dictionaries {names}')
        require((mozc / 'user_dictionary.db.bak-atok').read_bytes() == old, 'backup of the first file')
        r = run(home, 'install')
        require(r.returncode == 0 and len(dictionaries(db)) == 5, 'install again replaces, not adds')
        require((mozc / 'user_dictionary.db.bak-atok').read_bytes() == old, 'the backup is not overwritten')
        r = run(home, 'names', 'on')
        require(r.returncode == 0 and {n: e for n, e, _, _ in dictionaries(db)}['ATOK 人名辞書'], 'names on')
        r = run(home, 'status')
        require(r.returncode == 0 and 'ATOK 人名辞書\t3\ton' in r.stdout, 'status ' + r.stdout)
        r = run(home, 'remove')
        require(r.returncode == 0 and [n for n, _, _, _ in dictionaries(db)] == ['私の辞書'], 'remove')
        require(db.read_bytes() == old, 'remove leaves the original bytes')
        # No ~/.mozc: $XDG_CONFIG_HOME/mozc, else ~/.config/mozc (Mozc's rule).
        home2 = Path(tmp) / 'home2'
        home2.mkdir()
        r = run(home2, 'install', '--names')
        require(r.returncode == 0 and (home2 / '.config/mozc/user_dictionary.db').exists(), 'default ~/.config/mozc')
        require(all(e for _, e, _, _ in dictionaries(home2 / '.config/mozc/user_dictionary.db')), '--names: all on')
        require(not (home2 / '.config/mozc/user_dictionary.db.bak-atok').exists(), 'no backup without a file')
        r = run(home2, 'install', env={'XDG_CONFIG_HOME': str(Path(tmp) / 'xdg')})
        require(r.returncode == 0 and (Path(tmp) / 'xdg/mozc/user_dictionary.db').exists(), '$XDG_CONFIG_HOME/mozc')
        # Without the books (the gate fails): nothing written.
        home3 = Path(tmp) / 'home3'
        home3.mkdir()
        r = run(home3, 'install', env={'FDTP_DICT_FAKE_BOOKS': '0'})
        require(r.returncode == 1 and not (home3 / '.config').exists(), 'no books: nothing written')
        r = run(home3, 'install', env={'FDTP_DICT_BIN': str(Path(tmp) / 'none')})
        require(r.returncode == 1, 'no fdtp-dict: fails')
    print('atok mozc: ok')


if __name__ == '__main__':
    main()
