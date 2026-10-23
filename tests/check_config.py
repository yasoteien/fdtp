#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Stage 1c acceptance: ~/.fdtprc, /etc/fdtprc and the customizer (z / E).

Run from the repository root: python3 -B tests/check_config.py --build build
Every rc file, fixture and snapshot stays under BUILD/config-fixture; the
files are named by FDTP_RC_SYSTEM / FDTP_RC_USER, never the real ~/.fdtprc.
External commands are only inspected as requests.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from check import check_snapshot, info, notice, popup_text, require, text

ROOT = Path(__file__).resolve().parents[1]
# Revision 1 item 27: the customizer in a frame; the list on rows 5..19 at
# 25 rows, a line between the names and the values (column 2 + the longest
# variable name, 25 cells, + a gap), the values two columns after it.
VARIABLE_VALUE_COL = 30
LIST_ROWS = range(5, 20)
KEY_VALUE_COL = 17  # keys: names in 2..14, the line on 15


def labels_of(script):
    source = script.read_text()
    labels = re.findall(r'\bSNAP\s+#\s*([a-z0-9_]+)', source)
    require(len(labels) == len(set(labels)), f'{script}: duplicate labels')
    require(len(labels) == len(re.findall(r'\bSNAP\b', source)), f'{script}: every SNAP needs a label')
    return labels


class Acceptance:
    def __init__(self, build):
        self.build = build
        self.base = build / 'config-fixture'
        if self.base.exists():
            shutil.rmtree(self.base)
        self.base.mkdir(parents=True)
        self.executable = build / ('fdtp-preview' if sys.platform == 'darwin' else 'fdtp')
        self.files = self.base / 'files'
        self.make_files(self.files)
        self.snapshots = 0

    @staticmethod
    def make_files(d):
        (d / 'sub').mkdir(parents=True)
        (d / '.hidden').write_text('hidden\n')
        (d / 'a.txt').write_text('a' * 30)
        (d / 'b.c').write_text('b' * 20)
        (d / 'big.dat').write_text('x' * 5000)
        for i in range(70):
            (d / f'n{i:02d}').write_text('')

    def run(self, name, script, user, system=None, home=None):
        home = home or self.base / name
        home.mkdir(exist_ok=True)
        env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(self.build / 'tmp'), TZ='UTC', SHELL='/bin/sh',
                   VISUAL='visual-editor', EDITOR='env-editor',
                   FDTP_RC_USER=str(user), FDTP_RC_SYSTEM=str(system) if system else '')
        prefix = home / 'screen'
        for old in home.glob('screen-*'):
            old.unlink()
        script = ROOT / 'tests' / script
        labels = labels_of(script)
        subprocess.run([str(self.executable), '--headless', '--script', str(script), '--out', str(prefix),
                        '--time', '1700000000', str(self.files)],
                       check=True, cwd=ROOT.parent, env=env, stdout=subprocess.DEVNULL)
        paths = sorted(home.glob('screen-*.cells'))
        require(len(paths) == len(labels), f'{name}: {len(paths)} snapshots for {len(labels)} labels')
        self.snapshots += len(paths)
        print(f'  {name}: {len(paths)} snapshots')
        return {label: check_snapshot(p) for label, p in zip(labels, paths)}

    def rc(self, name, body):
        path = self.base / name
        path.write_text(body)
        return path


def rows(snap, first=0, last=25):
    return [text(snap, y) for y in range(first, last)]


def selected_row(snap):
    """The highlighted row of the customizer list (rows 5..19)."""
    for y in LIST_ROWS:
        if snap['rows'][y][2][2] == 2:  # WHITE background
            return text(snap, y)
    raise AssertionError('no selected row')


def path_row(snap):
    return re.match(r'Path=(\S+)', text(snap, 3).strip()).group(1)


def check_bind(t):
    user = t.rc('bind.rc', '\n'.join([
        '# bind tests',
        'bind d CUR_DOWN :下へ',
        'bind @A "echo %C %X %M in %P" :エコー',
        'bind o NO_OPERATION IN_DIR',
        '',
    ]))
    system = t.rc('bind-system.rc', 'bind d DELETE_FILE\nbind o COPY_FILE\n')
    before = user.read_text()
    s = t.run('bind', 'config_bind.keys', user, system)
    require(notice(s['start']) == '', f'bind/start: {notice(s["start"])!r}')
    require(info(s['start']).startswith('..'), 'bind/start: cursor on ..')
    require(info(s['rebound_down']).startswith('sub'), f'bind: d moves down (user overrides system): {info(s["rebound_down"])!r}')
    require(path_row(s['second_command_dir']).endswith('/files/sub'), f'bind: second command on a directory: {path_row(s["second_command_dir"])!r}')
    require(info(s['on_file']).startswith('a.txt'), f'bind/on_file: {info(s["on_file"])!r}')
    require(info(s['second_command_file']).startswith('a.txt') and path_row(s['second_command_file']).endswith('/files'),
            'bind: first command (NO_OPERATION) on a file')
    require(s['second_command_file']['depth'] == 1 and not s['second_command_file']['external']['pending'], 'bind: o on a file does nothing')
    ext = s['alt_a_shell']['external']
    require(ext['pending'] and ext['flags'] == 3, f'bind: Alt+A requests an external command: {ext}')
    require(ext['argv'] == ['/bin/sh', '-c', f'echo b.c b a.txt b.c in {t.files}'], f'bind: macros: {ext["argv"]}')
    require(ext['cwd'] == str(t.files), f'bind: cwd {ext["cwd"]!r}')
    require(not s['alt_b_empty']['external']['pending'] and s['alt_b_empty']['depth'] == 1, 'bind: Alt+B is an empty frame')
    row = selected_row(s['customizer_alt_a'])
    require(row[2:15].strip() == 'Alt+a' and row[KEY_VALUE_COL:].strip() == '"echo %C %X %M in %P" :エコー', f'customizer shows Alt+A: {row!r}')
    require(s['customizer_alt_a']['depth'] == 2, 'customizer is a screen')
    require(s['left_unwritten']['depth'] == 1, 'N leaves the customizer')
    require(user.read_text() == before and not Path(str(user) + '.bak').exists(), 'N does not write')


def five_columns(snap, row):
    return all(text(snap, row, k * 16, k * 16 + 16).strip() for k in range(5))


def check_set(t):
    system = t.rc('set-system.rc', 'DISPLAYMODE=1\nSORTTYPE=103\n')
    user = t.rc('set.rc', '\n'.join([
        'DISPLAYMODE=5',
        'set DOTFILE 1',
        'set FILETYPE=1',
        'EDITOR="my-editor --flag"',
        'SHELL=/bin/test-shell',
        'PAGER=less',
        '',
    ]))
    s = t.run('set', 'config_set.keys', user, system)
    start = s['start']
    require(notice(start) == '', f'set/start: {notice(start)!r}')
    require(five_columns(start, 6) and five_columns(start, 7), 'set: DISPLAYMODE=5 (user over system 1)')
    require(all('.hidden' not in r and 'hidden' not in r for r in rows(start, 5, 23)), 'set: DOTFILE=1 hides dot files')
    require(text(start, 24, 1, 3) == 'TH', f'set: FILETYPE and DOTFILE marks: {text(start, 24, 0, 4)!r}')
    require(text(start, 8, 0, 16).strip().startswith('big'), f'set: SORTTYPE=103 from the system file: {text(start, 8, 0, 16)!r}')
    require('sub/' in text(start, 7, 0, 16), f'set: FILETYPE suffix: {text(start, 7, 0, 16)!r}')
    ext = s['editor']['external']
    require(ext['argv'] == ['my-editor', '--flag', str(t.files / 'a.txt')], f'set: EDITOR before $VISUAL: {ext["argv"]}')
    ext = s['shell']['external']
    require(ext['argv'] == ['/bin/test-shell'], f'set: SHELL for Shift+F6: {ext["argv"]}')
    split = s['split']
    require(split['separators'], 'set: split')
    sep = next(y for y in range(6, 23) if split['separators'] >> y & 1)
    require(five_columns(split, sep + 1), 'set: the new pane takes DISPLAYMODE')
    v = rows(s['variables'], 5, 13)
    expect = {'EDITOR': 'my-editor --flag', 'PAGER': 'less', 'SHELL': '/bin/test-shell', 'SORTTYPE': '103',
              'DISPLAYMODE': '5', 'DOTFILE': '1', 'SYMLINK': '0', 'FILETYPE': '1'}
    for line, (name, value) in zip(v, expect.items()):
        require(line[2:VARIABLE_VALUE_COL].strip() == name and line[VARIABLE_VALUE_COL:].strip() == value,
                f'set: customizer {name}: {line!r}')


def check_variable_width(t):
    user = t.rc('variable-width.rc', 'EDITOR="' + 'x' * 80 + '"\n')
    # The NOVEL_FORMAT_ rows are a little above the end since SPLIT_SIZE and
    # UNDO_LEVELS came last (items 37, 38): look on the last page and the one
    # scrolled a few rows up from it.
    script = t.rc('variable-width.keys', 'z SNAP # first_page\nEND SNAP # last_page\n' + 'UP ' * 18 + 'SNAP # near_end\n')
    s = t.run('variable-width', script, user)
    first = s['first_page']
    require(text(first, 3, VARIABLE_VALUE_COL, VARIABLE_VALUE_COL + 2) == '値',
            'variable width: value heading follows the full name column')
    require(text(first, 5, VARIABLE_VALUE_COL, 78) == 'x' * (78 - VARIABLE_VALUE_COL),
            'variable width: long values use the remaining width')
    for name in ('NOVEL_FORMAT_EXCLAIM_KEEP', 'NOVEL_FORMAT_LEADER',
                 'NOVEL_FORMAT_LEADER2', 'NOVEL_FORMAT_LEADER_EVEN'):
        page = next((p for p in ('last_page', 'near_end')
                     if any(text(s[p], y, 2, VARIABLE_VALUE_COL).strip() == name for y in LIST_ROWS)), 'last_page')
        matches = [y for y in LIST_ROWS
                   if text(s[page], y, 2, VARIABLE_VALUE_COL).strip() == name]
        require(len(matches) == 1, f'variable width: full distinct name {name}')
        y = matches[0]
        require(text(s[page], y, VARIABLE_VALUE_COL, 78).strip() == '1',
                f'variable width: value does not overlap {name}')
    for snap in s.values():
        require(all(text(snap, y, 78, 80) == '  ' for y in LIST_ROWS),
                'variable width: list stays inside the 80-column screen')


def key_rows(snap):
    """The keys heading's rows on the screen: {label: value} (headings left out)."""
    out = {}
    for y in LIST_ROWS:
        label, value = text(snap, y, 2, 15).strip(), text(snap, y, KEY_VALUE_COL, 79).strip()
        if label and value:
            out[label] = value
    return out


def downs(n):
    return ' '.join(['DOWN'] * n)


def check_keys_list(t):
    """Revision 1 item 27: every key from the start, scrolled; Reserved; Alt+e."""
    user = t.rc('keys.rc', 'bind @Q "echo %C" :エコー\nbind ^G CUR_DOWN\n')
    pages = ''.join(f'PGDN SNAP # page_{i:02d}\n' for i in range(1, 13))
    # From HOME (a, item 1): a..z 1..26, a heading, Alt+a 28.., a heading, Ctrl+a 55...
    script = t.rc('keys.keys', 'z RIGHT SNAP # page_00\n' + pages + 'END SNAP # end\n'
                  'HOME DOWN DOWN DOWN DOWN SNAP # on_e\n'
                  'RET SNAP # e_asks\n'
                  'n SNAP # e_kept\n'
                  f'HOME {downs(30)} SNAP # on_alt_e\n'
                  'UP UP UP UP UP SNAP # over_heading\n'
                  'UP RET SNAP # y_input\n'
                  f'ESC HOME {downs(54)} SNAP # on_ctrl_c\n'
                  'RET SNAP # ctrl_c_fixed\n'
                  'ESC HOME DOWN DOWN DOWN DOWN RET y SNAP # e_override\n'
                  'C-u "VIEW_FILE" RET SNAP # e_viewer\n'
                  'RET C-u RET SNAP # e_back\n'
                  'ESC n SNAP # closed\n')
    s = t.run('keys', script, user)
    first = s['page_00']
    require(first['boxes'] == [[0, 0, 80, 23, 0, 1]], f'keys: the frame over rows 0..22 {first["boxes"]}')
    require('英字' in text(first, 5) and text(first, 6, 2, 15).strip() == 'a', f'keys: the first section {text(first, 5)!r}')
    require('↑↓ 1-15/' in text(first, 22), f'keys: where the list is, in the bottom line {text(first, 22)!r}')
    require('キー' in text(first, 3) and '割り当て' in text(first, 3), 'keys: column titles')
    seen = {}
    for label in ['page_00'] + [f'page_{i:02d}' for i in range(1, 13)] + ['end']:
        seen.update(key_rows(s[label]))
    total = int(re.search(r'/(\d+)', text(first, 22)).group(1))
    want = ([chr(c) for c in range(97, 123)] + [f'Alt+{chr(c)}' for c in range(97, 123)] +
            [f'Ctrl+{chr(c)}' for c in range(97, 123)] + [f'F{i}' for i in range(1, 13)] +
            [f'Shift+F{i}' for i in range(1, 13)] + ['<', '>', '/', '^', '*', '1', '5', 'Tab', 'Space', 'Ctrl+Space', 'Esc', 'Home', '￥'])
    missing = [k for k in want if k not in seen]
    require(not missing, f'keys: every key listed, missing {missing}')
    require(seen['e'] == '内蔵エディタ（Reserved）' and seen['F9'] == '内蔵エディタ（Reserved）', f"keys: e {seen['e']!r}")
    require(seen['Alt+e'] == '外部のエディタ（Reserved）' and seen['Ctrl+e'] == '外部のエディタ（Reserved）', f"keys: Alt+e {seen['Alt+e']!r}")
    require(seen['Alt+r'] == 'Retro MIDI Player（Reserved）', f"keys: Alt+r {seen['Alt+r']!r}")
    require(seen['Alt+a'] == '（空き）' and seen['g'] == '（空き）', 'keys: free keys')
    require(seen['Alt+q'] == '"echo %C" :エコー' and seen['Ctrl+g'] == 'CUR_DOWN（下へ）', f"keys: the rc's binds {seen['Alt+q']!r} {seen['Ctrl+g']!r}")
    require(seen['Ctrl+c'] == 'FDTP の即時終了（Reserved）', f"keys: Ctrl+c {seen['Ctrl+c']!r}")
    require(seen['Shift+F6'] == 'シェル（Reserved）', f"keys: Shift+F6 {seen['Shift+F6']!r}")
    end = s['end']
    require(selected_row(end).strip().startswith('（追加）') and f'/{total}' in text(end, 22), 'keys: END is 「（追加）」, the last item')
    require(selected_row(s['on_e'])[2:15].strip() == 'e' and 'bind e EDIT_NOVEL' in text(s['on_e'], 21), f"keys: the line under the list {text(s['on_e'], 21)!r}")
    require(popup_text(s['e_asks']).startswith('e は 内蔵エディタ（Reserved）です。割り当てを変えますか？'), f"keys: a built-in key asks {popup_text(s['e_asks'])!r}")
    require(s['e_kept']['widget'] == 0 and s['e_kept']['depth'] == 2 and key_rows(s['e_kept'])['e'] == '内蔵エディタ（Reserved）', 'keys: n keeps it')
    require(selected_row(s['on_alt_e'])[2:15].strip() == 'Alt+e', f"keys: Alt+e {selected_row(s['on_alt_e'])!r}")
    over = s['over_heading']
    require(selected_row(over)[2:15].strip() == 'z' and any('Alt+英字' in text(over, y) for y in LIST_ROWS), 'keys: ↑ goes over the heading')
    require(popup_text(s['y_input']).startswith('y の割り当て'), f"keys: a free key opens the input {popup_text(s['y_input'])!r}")
    require(selected_row(s['on_ctrl_c'])[2:15].strip() == 'Ctrl+c' and 'FDTP の即時終了' in popup_text(s['ctrl_c_fixed']) + text(s['ctrl_c_fixed'], 23),
            'keys: Ctrl+c cannot change')
    require(popup_text(s['e_override']).startswith('e の割り当て') and 'EDIT_NOVEL' in popup_text(s['e_override']), f"keys: y edits {popup_text(s['e_override'])!r}")
    require(key_rows(s['e_viewer'])['e'] == 'VIEW_FILE（表示）' and '変更あり' in text(s['e_viewer'], 0), f"keys: e overridden {key_rows(s['e_viewer'])}")
    require(key_rows(s['e_back'])['e'] == '内蔵エディタ（Reserved）', 'keys: an empty answer gives the built-in command back')
    require(s['closed']['depth'] == 1, 'keys: closed')


def check_awake(t):
    """Revision 1 item 28: AWAKE_MINUTES, 15 when not set, read, refused, written."""
    def value(snap, name='AWAKE_MINUTES'):
        y = next(y for y in LIST_ROWS if text(snap, y, 2, VARIABLE_VALUE_COL).strip() == name)
        return text(snap, y, VARIABLE_VALUE_COL, 79).strip()
    # SPLIT_SIZE and UNDO_LEVELS (items 37, 38) come after AWAKE_MINUTES.
    s = t.run('awake-default', t.rc('awake-default.keys', 'z END SNAP # end\nUP UP SNAP # last\nESC n\n'), t.base / 'none.rc')
    require(value(s['last']) == '15', f"awake: default {value(s['last'])!r}")
    require('最後のキーから' in text(s['last'], 21), f"awake: the explanation {text(s['last'], 21)!r}")
    require(value(s['end'], 'SPLIT_SIZE') == '10M' and value(s['end'], 'UNDO_LEVELS') == '100',
            f"split/undo: defaults {value(s['end'], 'SPLIT_SIZE')!r} {value(s['end'], 'UNDO_LEVELS')!r}")
    require('Ctrl+z' in text(s['end'], 21), f"undo: the explanation {text(s['end'], 21)!r}")
    user = t.rc('awake.rc', 'AWAKE_MINUTES=30\n')
    s = t.run('awake', t.rc('awake.keys', 'z END UP UP SNAP # read\n'
                            'RET C-u "1441" RET SNAP # refused\n'
                            'ESC RET C-u "x" RET SNAP # not_a_number\n'
                            'ESC RET C-u "1440" RET SNAP # most\n'
                            'RET C-u "0" RET SNAP # zero\n'
                            'ESC y SNAP # written\n'), user)
    require(value(s['read']) == '30', f"awake: read {value(s['read'])!r}")
    require(notice(s['refused']) .startswith('値が正しくありません') and value(s['refused']) == '30', 'awake: 1441 refused')
    require(notice(s['not_a_number']).startswith('値が正しくありません'), 'awake: x refused')
    require(value(s['most']) == '1440', 'awake: 1440 taken')
    require(value(s['zero']) == '0', 'awake: 0 taken')
    require(user.read_text() == 'AWAKE_MINUTES=0\n', f'awake: written {user.read_text()!r}')
    bad = t.rc('awake-bad.rc', 'AWAKE_MINUTES=-1\nAWAKE_MINUTES=2000\n')
    s = t.run('awake-bad', t.rc('awake-bad.keys', 'SNAP # start\nz END SNAP # last\n'), bad)
    require(notice(s['start']) == '2 行を読み飛ばしました' and value(s['last']) == '15', f"awake: invalid values skipped {notice(s['start'])!r}")


SKIP_RC = '''# lines FDTP does not read
alias ll='ls -l'
keymap UP "\\033[A"
if [ "$TERM" = linux ]; then
  bind x CUR_UP
fi
while true; do
  echo
done
myfunc() {
  bind y CUR_UP
}
export LANG=C
FOO=bar
EDITOR=$HOME/bin/vi
bind q UNKNOWN_COMMAND
arch .lzh "lha l"
ls | sort
bind d \\
  CUR_DOWN
bind c COPY_FILE :for you
'''


def check_skip(t):
    user = t.rc('skip.rc', SKIP_RC)
    s = t.run('skip', 'config_skip.keys', user)
    require(notice(s['start']) == '17 行を読み飛ばしました', f'skip: {notice(s["start"])!r}')
    require(info(s['continued_bind']).startswith('sub'), 'skip: a bind continued with \\ is read')
    require(s['upper_e']['depth'] == 2 and '変数' in text(s['upper_e'], 1), 'z opens the customizer (E dropped in revision 1)')
    require(s['back_to_customizer']['depth'] == 2 and notice(s['back_to_customizer']) == '', 'Esc on the question goes back')


CUSTOM_RC = '''# my settings
bind d CUR_DOWN
launch .pdf pdfview
alias ll='ls -l'
if [ -n "$X" ]; then
  bind x CUR_UP
fi

SORTTYPE=1 # by name
'''
WRITTEN_RC = '''# my settings
bind d DELETE_FILE
launch .pdf pdfview
# 読み飛ばし: alias ll='ls -l'
# 読み飛ばし: if [ -n "$X" ]; then
# 読み飛ばし:   bind x CUR_UP
# 読み飛ばし: fi

SORTTYPE=1 # by name
DISPLAYMODE=3
DOTFILE=1
bind @B "ls -l %P" :list
launch .xyz "xyz-open"
'''


def check_customize(t):
    home = t.base / 'custom'
    home.mkdir()
    user = home / 'fdtprc'
    user.write_text(CUSTOM_RC)
    bak = Path(str(user) + '.bak')
    s = t.run('customize', 'config_customize.keys', user, home=home)
    opened = s['opened']
    require(opened['depth'] == 2 and '変数' in text(opened, 1) and 'ランチャ' in text(opened, 1), 'customize: three headings')
    require(notice(opened) == '', 'customize: no notice in the editor')
    require('fdtprc' in text(opened, 1), f'customize: file shown: {text(opened, 1)!r}')
    require('FDTP の設定' in text(opened, 0) and opened['boxes'] == [[0, 0, 80, 23, 0, 1]], f'customize: the frame {opened["boxes"]}')
    require(selected_row(s['displaymode'])[2:].startswith('DISPLAYMODE') and selected_row(s['displaymode'])[VARIABLE_VALUE_COL:].strip() == '3',
            f'customize: DISPLAYMODE: {selected_row(s["displaymode"])!r}')
    require('変更あり' in text(s['displaymode'], 0), 'customize: changed mark (in the top line)')
    require(selected_row(s['dotfile'])[VARIABLE_VALUE_COL:].strip() == '1', 'customize: DOTFILE')
    require(notice(s['invalid_filetype']) == '値が正しくありません' and selected_row(s['invalid_closed'])[VARIABLE_VALUE_COL:].strip() == '0',
            f'customize: invalid value refused: {notice(s["invalid_filetype"])!r}')
    row = selected_row(s['added_alt_b'])
    require(row[2:15].strip() == 'Alt+b' and row[KEY_VALUE_COL:].strip() == '"ls -l %P" :list', f'customize: Alt+B added: {row!r}')
    launch = rows(s['added_launch'], 5, 20)
    require(any(r[2:20].strip() == '.pdf' and r[20:].strip() == 'pdfview' for r in launch), f'customize: launch .pdf listed: {launch[:4]}')
    require(any(r[2:20].strip() == '.xyz' and r[20:].strip() == '"xyz-open"' for r in launch), f'customize: launch .xyz added: {launch[:4]}')
    require(popup_text(s['confirm']).endswith('fdtprc に書き込みますか？ (Y/N)'), f'customize: confirm: {popup_text(s["confirm"])!r}')
    require(s['confirm_cancelled']['depth'] == 2 and notice(s['confirm_cancelled']) == '', 'customize: Esc returns to the editor')
    w = s['written']
    require(w['depth'] == 1 and notice(w).endswith('fdtprc に書き込みました'), f'customize: written: {notice(w)!r}')
    require(all(text(w, 6, k * 26, k * 26 + 26).strip() for k in range(3)), 'customize: 3 columns now')
    require(text(w, 24, 2, 3) == 'H', 'customize: dot files hidden now')
    require(s['alt_b_shell']['external']['argv'] == ['/bin/sh', '-c', f'ls -l {t.files}'], f'customize: Alt+B runs: {s["alt_b_shell"]["external"]}')
    require(user.read_text() == WRITTEN_RC, 'customize: written file:\n' + user.read_text())
    require(bak.read_text() == CUSTOM_RC, 'customize: .bak keeps the old file')
    require(s['closed']['depth'] == 1, 'customize: closed')
    # Read the written file again: same state, nothing skipped, same file when written again.
    r = t.run('reread', 'config_reread.keys', user, home=t.base / 'reread')
    require(notice(r['start']) == '', f'reread: nothing skipped: {notice(r["start"])!r}')
    # Row 1 holds the disk's free space, which other writes change.
    require(rows(r['start'], 2, 23) == rows(s['closed'], 2, 23), 'reread: the filer looks the same')
    for label in ('after_variables', 'after_keys', 'after_launch'):
        require(r[label]['rows'] == s[label]['rows'], f'reread: {label} differs')
    require(user.read_text() == WRITTEN_RC and bak.read_text() == WRITTEN_RC, 'reread: writing again changes nothing')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=ROOT / 'build')
    args = parser.parse_args()
    build = args.build.resolve()
    (build / 'tmp').mkdir(parents=True, exist_ok=True)
    t = Acceptance(build)
    require(t.executable.exists(), f'{t.executable}: build it first (make -C fdtp)')
    check_bind(t)
    check_set(t)
    check_variable_width(t)
    check_skip(t)
    check_customize(t)
    check_keys_list(t)
    check_awake(t)
    print(f'1c checks passed: {t.snapshots} snapshots')


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError, subprocess.CalledProcessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
