#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Pomera DM250: put the proper nouns of the stock ATOK dictionaries into
Mozc's user dictionary (revision 1 §16). The words come from fdtp-dict
(`fdtp-dict atok BOOK`, in the Pomera .deb; it reads the stock eMMC mounted
read-only at /mnt/pomera-dict and prints reading TAB word TAB PosType).

  pomera_atok_mozc.py install [--names]   add or replace the ATOK dictionaries
  pomera_atok_mozc.py remove              take them out again
  pomera_atok_mozc.py names on|off        use the personal names one or not
  pomera_atok_mozc.py status              what is there

The sports, anime and entertainment dictionaries are used; the personal names
one (about 150,000 names) is added switched off, because Mozc ranks user
dictionary words high and so many names push common words down (--names or
`names on` switches it on). Only Mozc dictionaries whose name starts with
"ATOK " are written; the user's own dictionaries and every other field of
user_dictionary.db stay as they are. The first write keeps a copy of the
file as user_dictionary.db.bak-atok. Run it as the user whose Mozc it is (not
root); mozc_server is restarted so that it reads the file again."""
import os
import random
import shutil
import subprocess
import sys

BOOKS = [('sports', 'ATOK スポーツ辞書'), ('anime', 'ATOK アニメ辞書'),
         ('entertainment', 'ATOK エンタメ辞書'), ('names', 'ATOK 人名辞書')]
NAMES = 'ATOK 人名辞書'
PREFIX = 'ATOK '


def say(ja, en):
    lang = os.environ.get('FDTP_MSG_LANG') or os.environ.get('LC_ALL') or os.environ.get('LC_MESSAGES') or os.environ.get('LANG', '')
    print(ja if lang.startswith('ja') else en)


def profile_dir():
    """Mozc's own rule (base/system_util.cc): ~/.mozc when it exists, else
    $XDG_CONFIG_HOME/mozc, else ~/.config/mozc."""
    home = os.path.expanduser('~')
    old = os.path.join(home, '.mozc')
    if os.path.isdir(old):
        return old
    xdg = os.environ.get('XDG_CONFIG_HOME')
    return os.path.join(xdg, 'mozc') if xdg else os.path.join(home, '.config', 'mozc')


def helper():
    for path in (os.environ.get('FDTP_DICT_BIN'), '/usr/bin/fdtp-dict', '/usr/local/bin/fdtp-dict',
                 os.path.expanduser('~/.fdtp/bin/fdtp-dict'), shutil.which('fdtp-dict')):
        if path and os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    return None


# ---- protobuf (UserDictionaryStorage, mozc/src/protocol/user_dictionary_storage.proto) ----
def varint(n):
    out = bytearray()
    while True:
        b, n = n & 0x7f, n >> 7
        out.append(b | 0x80 if n else b)
        if not n:
            return bytes(out)


def field(num, payload):
    if isinstance(payload, int):
        return varint(num << 3) + varint(payload)
    return varint(num << 3 | 2) + varint(len(payload)) + payload


def read_varint(b, i):
    n = s = 0
    while True:
        if i >= len(b):
            raise ValueError('truncated')
        c = b[i]
        i += 1
        n |= (c & 0x7f) << s
        s += 7
        if not c & 0x80:
            return n, i


def fields(b):
    """(number, wire type, value, raw bytes of the whole field)"""
    i = 0
    while i < len(b):
        start = i
        key, i = read_varint(b, i)
        wt = key & 7
        if wt == 0:
            value, i = read_varint(b, i)
        elif wt == 2:
            n, i = read_varint(b, i)
            value = b[i:i + n]
            if len(value) != n:
                raise ValueError('truncated')
            i += n
        elif wt == 1:
            value, i = b[i:i + 8], i + 8
        elif wt == 5:
            value, i = b[i:i + 4], i + 4
        else:
            raise ValueError('wire type %d' % wt)
        yield key >> 3, wt, value, b[start:i]


def dictionary_info(body):
    name, enabled, entries = '', True, 0
    for num, wt, value, _ in fields(body):
        if num == 3 and wt == 2:
            name = value.decode('utf-8', 'replace')
        elif num == 2 and wt == 0:
            enabled = bool(value)
        elif num == 4 and wt == 2:
            entries += 1
    return name, enabled, entries


def dictionary(name, rows, enabled):
    body = bytearray(field(1, random.getrandbits(62) | 1) + field(2, 1 if enabled else 0) + field(3, name.encode()))
    for key, value, pos in rows:
        body += field(4, field(1, key.encode()) + field(2, value.encode()) + field(5, pos))
    return field(2, bytes(body))


def load(path):
    return open(path, 'rb').read() if os.path.exists(path) else b''


def rewrite(path, keep, add):
    """keep(name) -> True to keep that ATOK dictionary as it is (others are
    always kept); add: [(name, rows, enabled)] appended."""
    old = load(path)
    out = bytearray()
    for num, wt, value, raw in fields(old):
        if num == 2 and wt == 2:
            name = dictionary_info(value)[0]
            if name.startswith(PREFIX) and not keep(name):
                continue
        out += raw
    for name, rows, enabled in add:
        out += dictionary(name, rows, enabled)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    backup = path + '.bak-atok'
    if old and not os.path.exists(backup):
        shutil.copy2(path, backup)
    tmp = path + '.tmp-atok'
    with open(tmp, 'wb') as f:
        f.write(out)
    os.replace(tmp, path)


def set_enabled(path, target, enabled):
    old = load(path)
    out = bytearray()
    found = False
    for num, wt, value, raw in fields(old):
        if num == 2 and wt == 2 and dictionary_info(value)[0] == target:
            found = True
            body = bytearray()
            for n2, w2, v2, r2 in fields(value):
                if n2 == 2 and w2 == 0:
                    continue
                body += r2
            out += field(2, field(2, 1 if enabled else 0) + bytes(body))
        else:
            out += raw
    if found:
        tmp = path + '.tmp-atok'
        with open(tmp, 'wb') as f:
            f.write(out)
        os.replace(tmp, path)
    return found


def restart_mozc():
    subprocess.run(['pkill', '-x', '-u', str(os.getuid()), 'mozc_server'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def words(exe, book):
    r = subprocess.run([exe, 'atok', book], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode:
        return None
    rows = []
    for line in r.stdout.decode('utf-8', 'replace').splitlines():
        parts = line.split('\t')
        if len(parts) == 3 and parts[0] and parts[1] and parts[2].isdigit():
            rows.append((parts[0], parts[1], int(parts[2])))
    return rows


def main(argv):
    if not argv or argv[0] not in ('install', 'remove', 'names', 'status'):
        print(__doc__.split('\n\n')[1])
        return 2
    if os.getuid() == 0 and not os.environ.get('FDTP_ATOK_MOZC_ROOT_OK'):
        say('利用者として実行してください（root の Mozc ではなく）。', 'Run it as the user, not root (Mozc is per user).')
        return 1
    path = os.path.join(profile_dir(), 'user_dictionary.db')
    try:
        if argv[0] == 'status':
            for num, wt, value, _ in fields(load(path)):
                if num == 2 and wt == 2:
                    name, enabled, n = dictionary_info(value)
                    print('%s\t%d\t%s' % (name, n, 'on' if enabled else 'off'))
            return 0
        if argv[0] == 'remove':
            rewrite(path, lambda name: False, [])
            restart_mozc()
            say('ATOK の辞書を Mozc から外しました。', 'The ATOK dictionaries were taken out of Mozc.')
            return 0
        if argv[0] == 'names':
            if len(argv) != 2 or argv[1] not in ('on', 'off'):
                return 2
            if not set_enabled(path, NAMES, argv[1] == 'on'):
                say('人名辞書がまだありません（先に install）。', 'The personal names dictionary is not there (install first).')
                return 1
            restart_mozc()
            return 0
        exe = helper()
        if not exe:
            say('fdtp-dict がありません（Pomera 用の .deb に入っています）。', 'fdtp-dict is missing (it comes with the Pomera .deb).')
            return 1
        add = []
        for book, name in BOOKS:
            rows = words(exe, book)
            if rows is None:
                say('ATOK の辞書を読めません（本体の eMMC の辞書が /mnt/pomera-dict に読み取り専用でマウントされていない）。',
                    'Cannot read the ATOK dictionaries (the stock eMMC is not mounted read-only at /mnt/pomera-dict).')
                return 1
            add.append((name, rows, book != 'names' or '--names' in argv))
        rewrite(path, lambda name: False, add)
        restart_mozc()
        for name, rows, enabled in add:
            say('%s: %d 語%s' % (name, len(rows), '' if enabled else '（切。names on で使う）'),
                '%s: %d words%s' % (name, len(rows), '' if enabled else ' (off; "names on" to use it)'))
        return 0
    except (OSError, ValueError) as error:
        say('失敗: %s' % error, 'Failed: %s' % error)
        return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
