#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Revision 1 B acceptance (docs/FDTP_REVISION1.md §1 1 2 8 9 10 11 18, §2, §3 14):
the new key assignments, run through the headless entrance with tests/keys_*.keys.

    python3 -B tests/check_keys.py --build build

Everything (fixtures, a fake rmp-jukebox, HOME) is written under BUILD/keys-check.
PDF's ↑↓ need libmupdf (Linux): tests/pdf_updown.keys in check_pdf.py.
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from check import screen_path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('office', ROOT / 'tools/make_test_office.py')
office = importlib.util.module_from_spec(spec)
spec.loader.exec_module(office)
BG, CYAN, WHITE, YELLOW = 0, 1, 2, 3
WIDGET = {'none': 0, 'notice': 1, 'confirm': 2, 'input': 3, 'choice': 4, 'info': 5, 'error': 6, 'help': 7}
checks = 0


def require(condition, message):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)


def text(snap, row, start=0, end=80):
    return ''.join(chr(c[0]) for c in snap['rows'][row][start:end] if c[3] != 2)


def info(snap):
    return text(snap, 4, 7, 24).rstrip()


def boxes(snap):
    """The text inside the floating boxes, row by row."""
    out = []
    for col, row, w, h, _rule, kind in snap.get('boxes', []):
        if not kind & 2:   # the frame of a whole screen (item 27), not a floating box
            continue
        out += [text(snap, y, col + 1, col + w - 1).strip() for y in range(row + 1, row + h - 1)]
    return [t for t in out if t]


def box_text(snap):
    return ' '.join(boxes(snap))


def info_values(snap):
    """「名前: 値」 rows of an information box; wrapped values are joined."""
    values, last = {}, None
    for line in boxes(snap)[1:]:
        m = re.match(r'^(\S+)\s*: (.*)$', line)
        if m:
            last = m.group(1); values[last] = m.group(2)
        elif last:
            values[last] += line
    return values


def fixture(base):
    if base.exists():
        shutil.rmtree(base)
    f = base / 'files'
    for d in ('alpha', 'apple', 'beta', 'sub/inner'):
        (f / d).mkdir(parents=True)
    (f / 'Banana.txt').write_text('banana\n')
    (f / 'bob.txt').write_text('bob\n')
    (f / 'notes.txt').write_text('既にあるメモ\n')
    (f / 'plain.dat').write_bytes(b'hello\n')
    (f / 'binary.dat').write_bytes(bytes(range(256)) * 4)
    (f / 'ctrl.dat').write_bytes(b'\x01\x02\x03\x04abc' * 50)
    (f / 'utf16.txt').write_bytes('UTF-16 の文\nA\tB\n'.encode('utf-16-le'))
    (f / 'image.png').write_bytes(office.png_bytes(8, 8, lambda x, y: (255, 0, 0)))
    (f / 'song.mid').write_bytes(b'MThd\x00\x00\x00\x06\x00\x00\x00\x01\x00\x60')
    (f / 'SONG.RCP').write_bytes(b'RCM-PC98V2.0(C)COME ON MUSIC\r\n' + bytes(64))
    (f / 'a.r36').write_bytes(b'RCM-PC98V2.0' + bytes(16))
    (f / 'b.G36').write_bytes(b'COME ON MUSIC' + bytes(16))
    (f / 'link').symlink_to('plain.dat')
    (f / '.hidden').write_text('h\n')
    slide = lambda word: office.pptx_slide([office.pptx_sp(2, 't', [office.pptx_para(word)], office.pptx_xfrm(500000, 500000, 6000000, 1000000))])
    office.package(f / 'deck.pptx', office.pptx_parts(b'', slides=[slide('一'), slide('二'), slide('三')], layouts_of=[2, 2, 2],
                                                      extra_slide_rels={1: []}, media=[]))
    tools = base / 'bin'
    tools.mkdir()
    (tools / 'rmp-jukebox').write_text('#!/bin/sh\nexit 0\n')
    (tools / 'rmp-jukebox').chmod(0o755)
    (base / 'home').mkdir()
    return f, tools


def run(build, base, executable, script, env, headless):
    f, _ = base / 'files', None
    source = (ROOT / 'tests' / script).read_text()
    labels = re.findall(r'\bSNAP\s+#\s*([a-z0-9_]+)', source)
    require(len(labels) == len(set(labels)) and len(labels) == len(re.findall(r'\bSNAP\b', source)), f'{script}: labels')
    prefix = base / 'out' / Path(script).stem
    prefix.parent.mkdir(exist_ok=True)
    for old in prefix.parent.glob(prefix.name + '-*'):
        old.unlink()
    subprocess.run([str(executable)] + (['--headless'] if headless else []) +
                   ['--script', str(ROOT / 'tests' / script), '--out', str(prefix), '--time', '1700000000', str(f)],
                   check=True, cwd=ROOT.parent, env=env, timeout=60)
    files = sorted(prefix.parent.glob(prefix.name + '-*.cells'))
    require(len(files) == len(labels), f'{script}: {len(files)} snapshots, {len(labels)} labels')
    return {label: json.loads(p.read_text()) for label, p in zip(labels, files)}


def check_open(s, f):
    e = s['e_binary']
    require(e['depth'] == 2 and 'binary.dat' in text(e, 0), f'e opens a binary file in the editor: {text(e, 0)!r}')
    # noveledit's own box (not this task's): undecodable bytes became U+FFFD.
    print('    e on binary.dat: ' + (box_text(e) or 'no notice'))
    require(s['f9_plain']['depth'] == 2 and 'plain.dat' in text(s['f9_plain'], 0), 'F9 opens the editor')
    d = s['e_directory']
    require(d['depth'] == 1 and d['widget'] == WIDGET['input'] and box_text(d).startswith('新しいファイル名'), f'e on a directory asks a name: {box_text(d)!r}')
    n = s['e_new_file']
    require(n['depth'] == 2 and 'new.txt' in text(n, 0), f'the editor opens the new name: {text(n, 0)!r}')
    require(not (f / 'new.txt').exists() and not (f / 'alpha/new.txt').exists(), 'no file until it is saved')
    x = s['e_existing']
    require(x['depth'] == 2 and 'notes.txt' in text(x, 0), 'e on .. with an existing name opens it')
    require((f / 'notes.txt').read_text() == '既にあるメモ\n', 'the existing file is kept')
    require(s['e_cancelled']['depth'] == 1 and s['e_cancelled']['widget'] == 0, 'Esc cancels the name')
    ext = s['ctrl_e']['external']
    require(ext['pending'] and ext['flags'] == 2 and ext['argv'] == ['test-editor', str(f / 'plain.dat')], f'C-e: the external editor {ext}')
    # Item 27: Alt+e asks the same external editor ($VISUAL, $EDITOR, vi).
    require(s['alt_e']['external'] == s['ctrl_e']['external'], f'Alt+e: the external editor {s["alt_e"]["external"]}')
    require(not s['alt_q_free']['external']['pending'] and s['alt_q_free']['depth'] == 1 and s['alt_q_free']['widget'] == 0, 'Alt+q is free')
    q = s['v_binary']
    rows = boxes(q)
    require(q['widget'] == WIDGET['choice'] and q['depth'] == 2, 'v on binary asks')
    require(''.join(rows[:-1]) == 'このファイルはバイナリ形式の可能性があります。参照方法を選んでください。', f'question text {rows!r}')
    require(rows[-1].split() == ['テキスト表示(T)', 'バイナリ表示(B)', 'キャンセル'], f'options {rows[-1]!r}')
    # The text view: the name in the frame's top line, 「行 N/M」 in its bottom line (row 22), no 16進.
    require(s['v_binary_text']['depth'] == 2 and 'binary.dat' in text(s['v_binary_text'], 0) and '行 ' in text(s['v_binary_text'], 22)
            and '16進' not in text(s['v_binary_text'], 22) and s['v_binary_text']['widget'] == 0, f'T: the text view {text(s["v_binary_text"], 22)!r}')
    h = s['v_binary_hex']
    require(h['depth'] == 2 and 'binary.dat' in text(h, 0) and '00 01 02 03' in ''.join(text(h, y) for y in range(1, 6)), 'B: the hex view')
    require(s['ret_binary']['widget'] == WIDGET['choice'], 'RET on binary asks too')
    require(s['ret_cancelled']['depth'] == 1 and s['ret_cancelled']['widget'] == 0, 'Esc: nothing opened')
    require(s['ret_cancel_option']['depth'] == 1 and s['ret_cancel_option']['widget'] == 0, 'キャンセル: nothing opened')
    require(s['ret_hex_by_arrows']['depth'] == 2 and '00 01 02 03' in ''.join(text(s['ret_hex_by_arrows'], y) for y in range(1, 6)), '→ RET picks バイナリ表示')
    require(s['ret_control']['widget'] == WIDGET['choice'], 'many control characters read as binary')
    for label in ('v_plain', 'v_utf16'):
        require(s[label]['depth'] == 2 and s[label]['widget'] == 0 and '行 ' in text(s[label], 22) and '16進' not in text(s[label], 22), f'{label}: text opens without asking')
    require(s['ret_image']['depth'] == 2 and s['ret_image']['widget'] != WIDGET['choice'], 'a known format (PNG) does not ask')
    for label, name in (('ret_mid', 'song.mid'), ('v_rcp', 'SONG.RCP'), ('ret_r36', 'a.r36'), ('ret_g36', 'b.G36'), ('ctrl_p', 'plain.dat'), ('alt_r', 'plain.dat')):
        ext = s[label]['external']
        require(ext['pending'] and ext['argv'] == ['rmp-jukebox', str(f / name)] and ext['flags'] == 0 and ext['cwd'] == str(f),
                f'{label}: rmp-jukebox request {ext}')
    require(not s['ctrl_p_directory']['external']['pending'] and 'ファイルを選んでください' in text(s['ctrl_p_directory'], 23), 'C-p on a directory')


def check_missing(s):
    m = s['missing_ret']
    require(m['widget'] == WIDGET['error'] and 'rmp-jukebox が見つかりません' in box_text(m) and not m['external']['pending'], f'no rmp-jukebox: {box_text(m)!r}')
    require(s['missing_closed']['widget'] == 0 and info(s['missing_closed']) == 'song.mid', 'the error box takes the key')
    require(s['missing_ctrl_p']['widget'] == WIDGET['error'], 'C-p without rmp-jukebox')


def check_info(s, f):
    v = info_values(s['info_plain'])
    require(s['info_plain']['widget'] == WIDGET['info'] and boxes(s['info_plain'])[0] == 'ファイルの情報', 'i: the file information box')
    names = ['名前', '場所', '種類', 'サイズ', '作成日時', '更新日時', 'アクセス日時', '属性', '持ち主とグループ', 'リンク先']
    require(list(v) == names, f'items {list(v)}')
    require(v['名前'] == 'plain.dat' and v['場所'] == str(f) and v['サイズ'] == '6 バイト（0.0 KB）', f'name/place/size {v}')
    require(v['種類'].startswith('テキスト（拡張子 .dat／中身 テキスト）'), v['種類'])
    st = os.lstat(f / 'plain.dat')
    import time
    require(v['更新日時'] == time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(st.st_mtime)), f'mtime {v["更新日時"]}')
    birth = getattr(st, 'st_birthtime', None)
    if birth is not None:
        require(v['作成日時'] == time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(birth)), f'birth time {v["作成日時"]}')
    else:
        require(re.match(r'^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d$', v['作成日時']) or v['作成日時'] == '不明', v['作成日時'])
    require(v['属性'] == 'rw-r--r--（なし）' or v['属性'].startswith('rw-'), v['属性'])
    require(v['リンク先'] == '（リンクではありません）', v['リンク先'])
    require(s['info_closed']['widget'] == 0 and info(s['info_closed']) == 'plain.dat', 'the info box takes the key that closes it')
    link = info_values(s['info_link'])
    require(link['リンク先'] == 'plain.dat' and link['種類'].startswith('シンボリックリンク → テキスト') and 'リンク' in link['属性'], f'link {link}')
    require(info_values(s['info_directory'])['種類'] == 'ディレクトリ', 'directory kind')
    require('中身 バイナリ' in info_values(s['info_binary'])['種類'], info_values(s['info_binary'])['種類'])
    require('MIDI' in info_values(s['info_mid'])['種類'] and '標準 MIDI' in info_values(s['info_mid'])['種類'], info_values(s['info_mid'])['種類'])
    require(s['fs_prompt']['widget'] == WIDGET['input'] and box_text(s['fs_prompt']).startswith('情報を調べるパス'), 'Shift+F2 asks the path')
    fs = s['fs_box']
    require(fs['widget'] == WIDGET['info'] and fs['depth'] == 1 and boxes(fs)[0] == 'ファイルシステムの情報', 'Shift+F2: the file system box')
    vfs = os.statvfs(f)
    require(str(vfs.f_blocks * (vfs.f_frsize or vfs.f_bsize)) in info_values(fs)['総容量'], info_values(fs))
    # Revision 1 item 25: a help box that scrolls (the whole list: check_help.py).
    for label in ('help_h', 'help_end', 'help_question'):
        help_text = '\n'.join(text(s[label], y) for y in range(25))
        require(s[label]['depth'] == 1 and s[label]['widget'] == WIDGET['help'] and 'FDTP キー一覧' in help_text, f'{label}: help')
        require('M-e' not in help_text and 'Alt+F' not in help_text, 'help: no M- notation')
    head = '\n'.join(text(s['help_h'], y) for y in range(25))
    tail = '\n'.join(text(s['help_end'], y) for y in range(25))
    for word in ('Shift+英字', 'C-Space', 'v Shift+F9', 'Enter'):
        require(word in head, f'help (top) mentions {word}')
    for word in ('Shift+F1', 'Attr', 'h ?', 'q Esc', 'F2（エディタ）'):
        require(word in tail, f'help (end) mentions {word}')
    require(s['help_closed']['depth'] == 1 and s['help_closed']['widget'] == 0, 'Esc closes the help')


def check_jump(s):
    order = [('top', '..'), ('b1', 'beta'), ('b2', 'Banana.txt'), ('b3', 'b.G36'), ('b4', 'binary.dat'), ('b5', 'bob.txt'),
             ('b_wrapped', 'beta'), ('a1', 'a.r36'), ('a2', 'alpha'), ('z_none', 'alpha'), ('s_from_plain', 'song.mid'),
             ('s_wrapped', 'sub'), ('old_keys_jump', 'link')]
    for label, name in order:
        require(info(s[label]) == name, f'{label}: {info(s[label])!r} != {name!r}')
        require(s[label]['depth'] == 1 and s[label]['widget'] == 0, f'{label}: nothing opened')


SHIFT_BAND = [' Attr ', ' Info ', ' Move ', 'rmDir ', 'mKdir ', 'sHell ', 'Write ', 'Backup', ' View ', ' Pack ']
SHIFT_HOT = [1, 1, 1, 2, 1, 1, 0, 0, 1, 1]
PLAIN_BAND = ['Logdsk', ' eXec ', ' Copy ', 'Delete', 'Rename', ' Sort ', ' Find ', ' Tree ', 'Editor', 'Unpack']
BAND_X = [4, 11, 18, 25, 32, 42, 49, 56, 63, 70]


def band(snap):
    return [text(snap, 24, x, x + 6) for x in BAND_X]


def check_shift(s):
    require(band(s['plain_band']) == PLAIN_BAND, band(s['plain_band']))
    b = s['shift_band']
    require(band(b) == SHIFT_BAND, f'Shift band {band(b)!r}')
    for k, x in enumerate(BAND_X):
        hot = b['rows'][24][x + SHIFT_HOT[k]]
        require(hot[2] == YELLOW and chr(hot[0]) == 'AIMDKHWBVP'[k], f'hot letter {k}: {chr(hot[0])}')
    require(band(s['band_back']) == PLAIN_BAND, 'band back after Shift')
    require(s['s_f1_attr']['depth'] == 2 and '属性' in ''.join(text(s['s_f1_attr'], y) for y in range(25)), 'Shift+F1: attributes')
    require(box_text(s['s_f2_info']).startswith('情報を調べるパス'), 'Shift+F2: file system')
    require(box_text(s['s_f3_move']).startswith('移動先'), 'Shift+F3: move')
    require('ディレクトリ beta' in box_text(s['s_f4_rmdir']) and '(Y/N)' in box_text(s['s_f4_rmdir']), 'Shift+F4: rmdir')
    require(box_text(s['s_f5_mkdir']).startswith('ディレクトリ名'), 'Shift+F5: mkdir')
    require(box_text(s['s_f6_shell']).startswith('シェル'), 'Shift+F6: shell')
    require('Linux では使えません' in box_text(s['s_f7_write']), 'Shift+F7: write')
    require(s['s_f8_backup']['depth'] == 2, 'Shift+F8: pack (Backup)')
    require(s['s_f9_view']['depth'] == 2 and 'plain.dat' in text(s['s_f9_view'], 0) and '行 ' in text(s['s_f9_view'], 22), 'Shift+F9: view')
    require(s['s_f10_pack']['depth'] == 2 and s['s_f10_pack']['rows'] == s['s_f8_backup']['rows'], 'Shift+F10: pack, the same as Shift+F8')
    require(box_text(s['f1_still_path']).startswith('移動先'), 'F1 without Shift: still the path')


def check_ctrl(s, f):
    require('ディレクトリ beta' in box_text(s['ctrl_d']), 'C-d: remove a directory')
    # Item 27: the search screen is framed (a box over the screen), the input over it.
    require(len(s['ctrl_f']['boxes']) == 2 and s['ctrl_f']['boxes'][0][:4] == [0, 0, 80, 23]
            and 'ディレクトリ検索' in text(s['ctrl_f'], 0)
            and box_text(s['ctrl_f']).startswith('ディレクトリのパターン'), 'C-f: find a directory')
    require(s['ctrl_a']['depth'] == 2 and '属性' in ''.join(text(s['ctrl_a'], y) for y in range(25)), 'C-a: attributes below')
    hidden = lambda snap: any('.hidden' in text(snap, y) for y in range(6, 23))
    require(text(s['ctrl_h'], 24, 2, 3) == 'H' and not hidden(s['ctrl_h']), 'C-h hides dot files')
    require(text(s['ctrl_h_again'], 24, 2, 3) != 'H' and hidden(s['ctrl_h_again']), 'C-h shows them again')
    require(bin(s['split']['separators']).count('1') == 1, 'split')
    require(s['widened']['separators'] != s['split']['separators'], 'C-w widens')
    require(s['narrowed']['separators'] != s['widened']['separators'], 'C-n narrows')
    require(s['killed']['separators'] == 0, 'C-k closes the window')


def check_tree(s, f):
    require(box_text(s['copy_prompt']).startswith('コピー先') and 'F8 ツリー' in box_text(s['copy_prompt']), 'copy prompt shows the F8 hint')
    t = s['copy_tree']
    require(t['depth'] == 3 and text(t, 0).strip() == 'コピー先のツリー' and t['widget'] == 0 and text(t, 21).strip() == screen_path(f),
            f'F8 opens the tree at the directory {text(t, 0)!r} {text(t, 21)!r}')
    b = s['copy_back']
    require(b['depth'] == 2 and b['widget'] == WIDGET['input'] and boxes(b)[1] == 'typed', f'Esc: the input as it was {boxes(b)!r}')
    require(text(s['copy_tree_moved'], 21).strip() == screen_path(f / 'alpha'), 'tree cursor moved to alpha')
    filled = s['copy_filled']
    require(filled['depth'] == 2 and filled['widget'] == WIDGET['input'] and str(f / 'alpha').endswith(boxes(filled)[1]) and boxes(filled)[1].endswith('files/alpha'), f'RET fills the field {boxes(filled)!r}')
    require((f / 'alpha/plain.dat').read_bytes() == b'hello\n', 'second RET copies')
    require(s['path_tree']['depth'] == 2 and '移動先のツリー' in text(s['path_tree'], 0), 'l F8: the tree')
    require(str(f / 'alpha').endswith(boxes(s['path_filled'])[1]) and boxes(s['path_filled'])[1].endswith('files/alpha'), 'path filled')
    require(text(s['path_moved'], 3, 7, 51).rstrip().endswith('files/alpha'), 'l moved into alpha')
    n = s['name_no_tree']
    require(n['depth'] == 2 and n['widget'] == WIDGET['input'] and box_text(n).startswith('ディレクトリ名') and 'F8' not in box_text(n), 'F8 does nothing in a name input')


def check_slide(s):
    for label, page in (('slide1', 1), ('slide2', 2), ('slide3', 3), ('slide3_end', 3), ('slide2_up', 2), ('slide1_top', 1)):
        require(s[label]['depth'] == 2 and 'スライド %d/3' % page in text(s[label], 22), f'{label}: {text(s[label], 22)!r}')   # the frame's bottom line
    require(s['back']['depth'] == 1, 'q leaves')


def check_settings(s):
    snap = s['link_target']
    require(info(snap) == 'plain.dat' and text(snap, 24, 0, 2) == 'ST', f'SYMLINK/FILETYPE from ~/.fdtprc {info(snap)!r} {text(snap, 24, 0, 2)!r}')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, required=True)
    args = ap.parse_args()
    build = args.build.resolve()
    subprocess.run(['make', '-C', str(ROOT), 'all', 'CFLAGS=-Werror', 'BUILD=' + str(build)], check=True, stdout=subprocess.DEVNULL)
    headless = sys.platform != 'darwin'
    executable = build / ('fdtp' if headless else 'fdtp-preview')
    base = build / 'keys-check'
    env0 = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(build / 'tmp'), TZ='UTC', VISUAL='test-editor', EDITOR='unused', FDTP_RC_SYSTEM='', FDTP_RC_USER='')
    (build / 'tmp').mkdir(exist_ok=True)

    def case(script, path_with_tool=True, rc=None):
        f, tools = fixture(base)
        env = dict(env0, HOME=str(base / 'home'), PATH=(str(tools) + ':' if path_with_tool else '') + '/usr/bin:/bin')
        if rc is not None:
            (base / 'fdtprc').write_text(rc)
            env['FDTP_RC_USER'] = str(base / 'fdtprc')
        s = run(build, base, executable, script, env, headless)
        print(f'  {script}: {len(s)} snapshots')
        return s, f

    s, f = case('keys_open.keys'); check_open(s, f)
    if shutil.which('rmp-jukebox', path='/usr/bin:/bin'):
        print('  keys_missing.keys: skipped (rmp-jukebox is in /usr/bin or /bin)')
    else:
        s, f = case('keys_missing.keys', path_with_tool=False); check_missing(s)
    s, f = case('keys_info.keys'); check_info(s, f)
    s, f = case('keys_jump.keys'); check_jump(s)
    s, f = case('keys_shift.keys'); check_shift(s)
    s, f = case('keys_ctrl.keys'); check_ctrl(s, f)
    s, f = case('keys_tree.keys'); check_tree(s, f)
    s, f = case('keys_slide.keys'); check_slide(s)
    s, f = case('keys_settings.keys', rc='SYMLINK=1\nFILETYPE=1\n'); check_settings(s)
    print(f'revision 1 B key checks passed: {checks} checks')


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, subprocess.CalledProcessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
