#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""T3 cells and file acceptance checks; generated data remains under BUILD."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BG, CYAN, WHITE, YELLOW = range(4)


def require(value, message):
    if not value:
        raise AssertionError(message)


def text(snap, row, start=0, end=80):
    return ''.join(chr(c[0]) for c in snap['rows'][row][start:end] if c[3] != 2)


def screen(snap):
    """Whole screen text: notices and windows may be a row or a floating box."""
    return '\n'.join(text(snap, y) for y in range(25))


# Revision 1 item 29: the screen's frame (box 0) over rows 0..22, the body in
# rows 1..21 and columns 1..78, the title in the top line, the status (the
# code, 「行 N/M」) in the bottom line (row 22).
TOP, LAST, STATUS = 1, 21, 22


def status(snap):
    return text(snap, STATUS)


def vlines(snap):
    """Columns of the vertical lines of the frame that join its top and bottom lines."""
    return sorted(l[0] for l in snap.get('lines', []) if l[3] == 1 and l[4] == 0 and l[1] == 0 and l[2] == STATUS)


def body(snap, row):
    """Text body right of the line numbers and their line (from column 1 without them)."""
    rules = vlines(snap)
    return text(snap, row, rules[0] + 1 if rules else 1, 79)


def ppm_column_cyan(path, x):
    """Rows of the PPM in which the pixel at grid x (80 columns: no margin) is FD cyan."""
    data = path.read_bytes()
    fields, at = [], 0
    while len(fields) < 4:
        while data[at:at + 1].isspace():
            at += 1
        end = at
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[at:end]); at = end
    width, height = int(fields[1]), int(fields[2])
    gx = (width - 960) // 2
    pixels = data[at + 1:]
    # Above the message line and the band (rows 23, 24).
    return [y for y in range(552) if pixels[(y * width + gx + x) * 3:(y * width + gx + x) * 3 + 3] == b'\x00\xff\xff']


def ppm_cyan_rows(path):
    """Rows of the PPM in which at least 900 pixels are FD cyan."""
    data = path.read_bytes()
    fields, at = [], 0
    while len(fields) < 4:
        while data[at:at + 1].isspace():
            at += 1
        end = at
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[at:end]); at = end
    width, height = int(fields[1]), int(fields[2])
    pixels = data[at + 1:]
    rows = []
    for y in range(height):
        line = pixels[y * width * 3:(y + 1) * width * 3]
        if sum(1 for x in range(width) if line[x * 3:x * 3 + 3] == b'\x00\xff\xff') >= 900:
            rows.append(y)
    return rows


def snapshot(path):
    data = json.loads(path.read_text())
    require(len(data['rows']) == 25, f'{path}: expected 25 rows')
    for y, row in enumerate(data['rows']):
        require(len(row) == 80, f'{path}: row width')
        for x, cell in enumerate(row):
            require(len(cell) == 4, f'{path}: cell shape')
            cp, fg, bg, part = cell
            require(0 <= cp <= 0x10ffff and not 0xd800 <= cp <= 0xdfff, f'{path}: scalar')
            require(0 <= fg < 9 and 0 <= bg < 9 and part in (0, 1, 2), f'{path}: palette/part')
            if part == 1:
                require(x < 79 and row[x + 1] == [cp, fg, bg, 2], f'{path}: wide head {x},{y}')
            if part == 2:
                require(x and row[x - 1] == [cp, fg, bg, 1], f'{path}: wide tail {x},{y}')
    return data


def fixture(build):
    directory = build / 'viewers-fixture'
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir()
    lines = ['日本語 UTF-8', 'A\tB', 'CTRL:\x01\x1b\x7f']
    lines += [f'row {i:03d}' + (' needle' if i in (5, 30) else '') for i in range(60)]
    (directory / 'f00.txt').write_bytes(('\n'.join(lines) + '\n').encode())
    japanese = '日本語 あいうえお 漢字 ｱｲｳ\n'
    (directory / 'f01.txt').write_bytes(japanese.encode('shift_jis'))
    (directory / 'f02.txt').write_bytes(japanese.encode('euc_jp'))
    (directory / 'f03.txt').write_bytes(b'')
    with (directory / 'f04.txt').open('wb') as file:
        file.write(b'SPARSE\n')
        file.truncate(1_000_000_000)
        file.seek(999_999_984)
        file.write(b'LAST-SPARSE-END!')
    original = bytearray(range(256)) * 2
    original[32:34] = b'AB'
    original[64:66] = b'AB'
    (directory / 'f05.txt').write_bytes(original)
    (directory / 'f06.txt').write_bytes('①㈱髙﨑\n'.encode('cp932'))
    (directory / 'f07.txt').write_bytes(b'CRLF\r\nCR\rLF\nLAST')
    (directory / 'f08.txt').write_bytes((''.join(chr(65 + i) * 8 for i in range(15)) + '\nTAIL\n').encode())
    (directory / 'f09.txt').write_text(''.join(f'line {i:04d}\n' for i in range(600)), encoding='utf-8')
    (directory / 'f10.txt').write_bytes(b'a' * 65535 + '日本語\n'.encode())
    (directory / 'f11.txt').write_text('A' * 77 + '日\tY\n', encoding='utf-8')
    for name, data in code_files().items():
        (directory / name).write_bytes(data)
    (directory / 'u1-units.bin').write_bytes('# テスト文字\n'.encode())
    (directory / 'u2-straddle.txt').write_bytes(b'A' * 15 + 'あ'.encode() + b'B\n')
    (directory / 'u3-edit.bin').write_bytes(bytes(range(16)))
    return directory, bytes(original)


# The same text in the seven encodings of the c window (c7 has no BOM).
CODE_TEXT = ['日本語 かな ｱｲｳ', 'A\tB 漢字']


def iso2022(text):
    """ISO-2022-JP with the CP932 NEC row (ESC $ B) for characters JIS lacks."""
    out, state = b'', 'B'
    for ch in text:
        try:
            data = ch.encode('iso2022_jp_ext')
        except UnicodeEncodeError:
            j = jis_from_sjis(ch.encode('cp932'))
            data = b'\x1b$B' + bytes(j) + b'\x1b(B'
        out += data
    return out


def jis_from_sjis(data):
    s1, s2 = data
    row = (s1 - 0x81) * 2 + 0x21 if s1 < 0xa0 else (s1 - 0xe0) * 2 + 0x5f
    if s2 >= 0x9f:
        return [row + 1, s2 - 0x7e]
    return [row, s2 - (0x20 if s2 >= 0x80 else 0x1f)]


def code_files():
    text = '\n'.join(CODE_TEXT) + '\n'
    jis_lines = [iso2022(CODE_TEXT[0] + ' ①¥'), iso2022(CODE_TEXT[1])]
    # Enough JIS lines to page, each starting and ending in the kanji set.
    jis_lines += [iso2022(f'漢字{i:02d}') for i in range(30)] + [iso2022('漢字 needle 丂')]
    return {
        'c1-sjis.txt': text.encode('cp932'),
        'c2-jis.txt': b'\n'.join(jis_lines) + b'\n',
        'c3-euc.txt': text.encode('euc_jp'),
        'c4-utf8.txt': text.encode('utf-8'),
        'c5-utf16le.txt': b'\xff\xfe' + text.encode('utf-16-le'),
        'c6-utf16be.txt': b'\xfe\xff' + text.encode('utf-16-be'),
        'c7-nobom.txt': text.encode('utf-16-le'),
    }


def run(build, directory):
    exe = build / ('fdtp-preview' if sys.platform == 'darwin' else 'fdtp')
    require(exe.is_file(), f'先に make -C fdtp を実行してください: {exe}')
    env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(build / 'tmp'), TZ='UTC')
    (build / 'tmp').mkdir(exist_ok=True)
    result = {}
    for script in sorted((ROOT / 'tests').glob('viewers_*.keys')):
        labels = re.findall(r'\bSNAP\s+#\s*([a-z0-9_]+)', script.read_text())
        require(len(labels) == len(set(labels)), f'{script}: duplicate labels')
        prefix = directory / script.stem
        command = [str(exe)] + ([] if sys.platform == 'darwin' else ['--headless'])
        command += ['--script', str(script), '--out', str(prefix), '--time', '1700000000', str(directory)]
        subprocess.run(command, cwd=ROOT.parent, env=env, check=True, timeout=30)
        files = sorted(directory.glob(script.stem + '-*.cells'))
        require(len(files) == len(labels), f'{script}: snapshot count {len(files)} != {len(labels)}')
        result[script.stem] = dict(zip(labels, (snapshot(path) for path in files)))
        result[script.stem]['_ppm'] = dict(zip(labels, (path.with_suffix('.ppm') for path in files)))
        print(f'  {script.name}: {len(labels)} snapshots')
    return result


def check_rules(snap, columns, label):
    """The frame (box 0 over rows 0..22) and its vertical lines down the given
    columns, joining its top and bottom lines (┬ ┴)."""
    require(snap['boxes'] and snap['boxes'][0][:4] == [0, 0, 80, 23] and snap['boxes'][0][5] == 1, f'{label}: the frame {snap["boxes"]}')
    require(vlines(snap) == columns, f'{label}: vertical lines {vlines(snap)} != {columns}')


def check_text(s):
    initial = s['initial']
    require(initial['depth'] == 2 and 'f00.txt' in text(initial, 0) and initial['rows'][0][9][1] == YELLOW, 'text title/screen')
    require('UTF-8（自動）' in status(initial) and '行 1/' in status(initial), 'UTF-8 detection/status')
    # The frame's top line (y 11-12) and bottom line (539-540); the line down
    # column 5 (x 65-66) from the top line to the bottom line.
    left = ppm_column_cyan(s['_ppm']['initial'], 5)
    require(set(range(18, 534)) <= set(left) and max(left) < 541, f'the frame left line {left[:3]}..{left[-3:]}')
    require(ppm_column_cyan(s['_ppm']['initial'], 65) == list(range(11, 541)), 'the line between the numbers and the text joins the frame')
    check_rules(initial, [5], 'text')
    require(text(initial, 1, 1, 5) == '   1' and initial['rows'][1][4][1] == CYAN, 'line number 1, 4 columns, cyan')
    require(text(initial, LAST, 1, 5) == '  21', 'line number 21')
    require(body(initial, 1).startswith('日本語 UTF-8'), 'UTF-8 Japanese')
    require(body(initial, 2).startswith('A       B'), 'eight-column TAB')
    require(body(initial, 3).startswith('CTRL:^A^[^?'), 'control notation')
    require(all(initial['rows'][3][x][1] == CYAN for x in range(11, 17)), 'control color')
    require(body(s['down'], 1).startswith('A       B') and text(s['down'], 1, 1, 5) == '   2', 'down one row')
    require(body(s['page_down'], 1).startswith('row 018'), 'page down 21 rows')
    require(s['page_down']['rows'] == s['space_page']['rows'], 'SPC advances page')
    require(body(s['first_match'], 1).startswith('row 005 needle'), 'search first')
    require(body(s['next_match'], 1).startswith('row 030 needle'), 'search next')
    require(body(s['previous_match'], 1).startswith('row 005 needle'), 'search previous')
    require('見つかりません' in screen(s['no_match']), 'search missing notice')
    require('検索:' not in screen(s['search_cancel']), 'search escape')
    require('row 059' in ''.join(text(s['bottom'], y) for y in range(TOP, LAST + 1)), 'bottom')
    require(body(s['top'], 1).startswith('日本語'), 'top')
    off = s['numbers_off']
    require(text(off, 1, 1).startswith('日本語 UTF-8') and not vlines(off), 'line numbers off')
    require(not set(range(24, 528)) <= set(ppm_column_cyan(s['_ppm']['numbers_off'], 65)), 'no line between numbers and text without numbers')
    require(s['numbers_on']['rows'] == initial['rows'], 'line numbers back on')
    # The choice is a row or a box (widgets decide); the first options always show.
    window = ''.join(text(s['code_window'], y) for y in range(25))
    for option in ('文字コード', '自動', 'Shift_JIS', 'JIS（ISO-2022-JP）', 'EUC-JP'):
        require(option in window, f'code window option {option}: {window!r}')
    for label, encoding in [('code_sjis', 'Shift_JIS'), ('code_euc', 'EUC-JP'), ('code_cancel', 'EUC-JP'), ('code_auto', 'UTF-8（自動）')]:
        require(encoding in status(s[label]), f'code choice {label}')
    require('自動' not in status(s['code_sjis']), 'chosen code is not automatic')
    require(not body(s['code_sjis'], 1).startswith('日本語'), 'Shift_JIS reading of UTF-8 differs')
    require(body(s['code_auto'], 1).startswith('日本語 UTF-8'), 'automatic restores UTF-8')
    require('Top' in text(initial, 24) and 'Quit' in text(initial, 24) and '#Line' in text(initial, 24), 'text bottom band')
    require(s['returned']['depth'] == 1, 'text quit')


def check_codes(s):
    names = {'sjis': 'Shift_JIS', 'jis': 'ISO-2022-JP', 'euc': 'EUC-JP', 'utf8': 'UTF-8',
             'utf16le': 'UTF-16LE', 'utf16be': 'UTF-16BE'}
    for label, name in names.items():
        snap = s[label]
        require(name + '（自動）' in status(snap), f'{label}: detected {name}')
        first = CODE_TEXT[0] + (' ①¥' if label == 'jis' else '')
        require(body(snap, 1).rstrip() == first, f'{label}: first line {body(snap, 1)!r}')
        require(body(snap, 2).rstrip() == CODE_TEXT[1].replace('\t', '       '), f'{label}: second line {body(snap, 2)!r}')
        require(text(snap, 2, 1, 5) == '   2', f'{label}: line numbers')
    require(body(s['jis_search'], 1).startswith('漢字 needle 丂'), 'JIS: search after shifts, JIS X 0212')
    require(body(s['jis_page'], 1).startswith('漢字') or body(s['jis_page'], 1) == ' ' * 73, 'JIS: page keeps kanji state')
    require(body(s['jis'], LAST).startswith('漢字18'), 'JIS: 21 rows of kanji lines')
    require('UTF-16' not in status(s['nobom_auto']), 'no BOM: UTF-16 not guessed')
    require('UTF-16LE' in status(s['nobom_le']) and '自動' not in status(s['nobom_le']), 'no BOM: chosen UTF-16LE')
    require(body(s['nobom_le'], 1).rstrip() == CODE_TEXT[0], 'no BOM: UTF-16LE text')
    # Hex (item 29): address 1..8, a line down column 9, hex from 11, a line
    # down column 60, characters from 61.
    hj = s['hex_jis']
    require('ISO-2022-JP（自動）' in status(hj), 'hex: JIS detected')
    require(text(hj, 1, 61, 64) == '...', 'hex: escape sequence bytes are dots')
    require(hj['rows'][1][64][0] == ord('日') and hj['rows'][1][64][3] == 1 and hj['rows'][1][72][0] == ord('本') and text(hj, 1, 66, 69) == '...', 'hex: JIS kanji at their first byte')
    hu = s['hex_utf16']
    require(text(hu, 1, 61, 63) == '..', 'hex: UTF-16 BOM dots')
    require(hu['rows'][1][63][0] == ord('日') and hu['rows'][1][65][0] == ord('本'), 'hex: UTF-16 characters')


def check_units(s):
    require(text(s['byte'], 1, 11, 59).rstrip() == '23 20 E3 83 86 E3 82 B9  E3 83 88 E6 96 87 E5 AD', 'byte units, uppercase')
    require(text(s['word'], 1, 11, 59).rstrip() == '2320 E383 86E3 82B9  E383 88E6 9687 E5AD', 'word units')
    require('ワード' in status(s['word']), 'word heading')
    require(text(s['word_swap'], 1, 11, 59).rstrip() == '2023 83E3 E386 B982  83E3 E688 8796 ADE5', 'word swapped')
    require('入替' in status(s['word_swap']), 'swap heading')
    require(text(s['dword_swap'], 1, 11, 59).rstrip() == '83E32023 B982E386  E68883E3 ADE58796', 'double word swapped')
    require(text(s['dword'], 1, 11, 59).rstrip() == '2320E383 86E382B9  E38388E6 9687E5AD', 'double word')
    require('ダブルワード' in status(s['dword']) and '入替' not in status(s['dword']), 'double word heading')
    require(s['byte_again']['rows'][TOP:LAST + 1] == s['byte']['rows'][TOP:LAST + 1], 'back to bytes')
    require('ワードかダブルワード' in ''.join(text(s['swap_refused'], y) for y in range(25)), 'swap needs word units')
    check_rules(s['byte'], [9, 60], 'hex')
    for x in (9 * 12 + 5, 60 * 12 + 5):
        require(ppm_column_cyan(s['_ppm']['byte'], x) == list(range(11, 541)), f'hex: the line at x {x} joins the frame')
    row = s['byte']['rows'][1]
    # '# テスト文字': a full-width character at its first byte, the rest blank.
    require(text(s['byte'], 1, 61, 63) == '# ', 'UTF-8 ASCII characters')
    for col, ch in [(2, 'テ'), (5, 'ス'), (8, 'ト'), (11, '文'), (14, '字')]:
        require(row[61 + col][0] == ord(ch) and row[61 + col][3] == 1, f'UTF-8 {ch} at byte {col}')
        require(row[63 + col][0] == ord(' '), f'UTF-8 {ch}: third byte blank')
    st = s['straddle']
    # A wide character begun on byte 15 takes columns 76-77, inside the frame.
    require(st['rows'][1][76][0] == ord('あ') and st['rows'][1][76][3] == 1, 'straddling character at its first byte')
    require(text(st, 2, 61, 64) == '  B', 'straddling character: continuation bytes blank')
    require('Shift_JIS' in status(s['straddle_sjis']) and s['straddle_sjis']['rows'][1][76][0] != ord('あ'), 'straddle as Shift_JIS')
    se = s['swap_edit']
    require(text(se, 1, 11, 59).rstrip() == '01AB CD02 0504 0706  0908 0B0A 0D0C 0F0E', f'edit in swapped words {text(se, 1, 11, 59)!r}')
    # Cursor (reversed colours) on offset 5 = the first slot of the third word.
    require(s['swap_move']['rows'][1][21][2] != BG and s['swap_move']['rows'][1][23][2] == BG, 'visual cursor movement in swapped words')
    require('書き込みました' in screen(s['swap_saved']), 'save with Ctrl+s')


def check_formats(s):
    for label, encoding in [('sjis', 'Shift_JIS'), ('euc', 'EUC-JP')]:
        require(encoding in status(s[label]), f'{encoding}: detection')
        require(body(s[label], 1).startswith('日本語 あいうえお 漢字 ｱｲｳ'), f'{encoding}: conversion')
    require(all(not body(s['empty'], y).strip() and not text(s['empty'], y, 1, 5).strip() for y in range(TOP, LAST + 1)), 'empty text body')
    require(body(s['cp932'], 1).startswith('①㈱髙﨑'), 'CP932 NEC/IBM decoding')
    require([body(s['newlines'], y).strip() for y in range(1, 5)] == ['CRLF', 'CR', 'LF', 'LAST'], 'CRLF/CR/LF splitting')
    groups = [chr(65 + i) * 8 for i in range(15)]
    require(body(s['wrapped'], 1) == ''.join(groups)[:73], 'long line first row (73 columns)')
    require(body(s['wrapped'], 2).startswith(''.join(groups)[73:]), 'long line second row')
    require(text(s['wrapped'], 2, 1, 5) == '    ' and text(s['wrapped'], 3, 1, 5) == '   2', 'number only on a line\'s first row')
    require(body(s['unwrapped'], 2).startswith('TAIL'), 'unwrap long line')
    require(body(s['right'], 1) == ''.join(groups)[8:81], 'horizontal right')
    require(s['unwrapped']['rows'] == s['left']['rows'], 'horizontal left restores')
    require(s['wrapped']['rows'] == s['rewrapped']['rows'], 'wrap restored')
    require(text(s['wrapped_plain'], 1, 1, 79) == ''.join(groups[:10])[:78], 'wrap at 78 without numbers')


def check_sparse(s, directory):
    require((directory / 'f04.txt').stat().st_size == 1_000_000_000, 'sparse size')
    require(body(s['text_sparse'], 1).startswith('SPARSE') and '/?' in status(s['text_sparse']), 'large text lazy indexing')
    require(text(s['hex_sparse'], 1, 1).startswith('00000000'), 'large hex first page')
    for label in ('hex_last', 'hex_goto_last'):
        require('LAST-SPARSE-END!' in ''.join(text(s[label], y) for y in range(TOP, LAST + 1)), f'{label}: final sparse bytes')
    require('範囲外' in screen(s['bad_goto']), 'invalid goto')


def check_hex(s, directory, original):
    require(s['initial']['depth'] == 2 and '16進' in status(s['initial']), 'hex screen')
    require(text(s['initial'], 1, 1).startswith('00000000  00 01 02 03 04 05 06 07  08'), f"hex layout/grouping {text(s['initial'], 1)!r}")
    require(s['initial']['rows'][1][1][1] == WHITE, 'address WHITE')
    require(text(s['edited'], 1, 11, 19) == '41 5A B1', 'hex/ASCII/kana editing')
    require(all(s['edited']['rows'][1][x][1] == YELLOW for x in (11, 12, 14, 15, 17, 18)), 'changed bytes YELLOW')
    require('書き込みますか？' in screen(s['save_prompt']), 'save confirmation')
    require('変更あり' in text(s['save_cancel'], 0), 'cancel save retains changes')
    require('書き込みました' in screen(s['saved']) and '変更あり' not in text(s['saved'], 0), 'confirmed save')
    for label, line in [('text_match', '行 3/'), ('next_match', '行 5/'), ('hex_match', '行 3/')]:
        require(line in status(s[label]), f'{label}: search location')
    require('偶数桁' in screen(s['bad_search']), 'invalid hex query')
    require('書き込まずに戻りますか？' in screen(s['discard_prompt']), 'discard confirmation')
    require(s['discard_cancel']['depth'] == 2 and '変更あり' in text(s['discard_cancel'], 0), 'cancel discard')
    require(s['returned']['depth'] == 1 and '変更あり' not in text(s['reopened'], 0), 'discard and reopen')
    expected = b'AZ\xb1' + original[3:]
    require((directory / 'f05.txt').read_bytes() == expected, 'saved file bytes/length; discarded edit absent')
    require('ｱ' in text(s['reopened'], 1, 61), 'halfwidth kana ASCII column')


def check_edges(s):
    # 13 pages of 21 rows (item 29) pass display row 256; one back crosses it again.
    require(body(s['checkpoint_forward'], 1).startswith('line 0273'), 'forward beyond display checkpoint 256')
    require(body(s['checkpoint_backward'], 1).startswith('line 0252'), 'backward across display checkpoint 256')
    require(text(s['numbers_keep_place'], 1, 1).startswith('line 0252'), 'line numbers off keeps the top line')
    require(body(s['indexed_bottom'], 1).startswith('line 0579') and body(s['indexed_bottom'], LAST).startswith('line 0599'), 'bottom beyond checkpoints')
    require(text(s['indexed_bottom'], LAST, 1, 5) == ' 600', 'line number 600')
    require(body(s['indexed_top'], 1).startswith('line 0000'), 'top after indexed bottom')
    require(body(s['indexed_search'], 1).startswith('line 0550'), 'search using indexed checkpoint')
    require('UTF-8' in status(s['sample_boundary']), 'UTF-8 scalar straddles detection sample boundary')
    # f11.txt: 77 A, then 日 (two columns) where one column is left.
    require(body(s['wide_numbered'], 1) == 'A' * 73, 'wrap at 73 columns with numbers')
    require(text(s['wide_boundary'], 1, 1, 79) == 'A' * 77 + ' ', 'wide scalar wraps intact at column 78')
    require(s['wide_boundary']['rows'][2][1][3] == 1 and s['wide_boundary']['rows'][2][2][3] == 2, 'wrapped wide scalar cell pair')
    # 日 ends at physical column 79: the TAB goes to 80 (one blank), not 8.
    require(text(s['wide_boundary'], 2, 1).startswith('日 Y'), 'TAB retains physical column after wide wrap')


def check_jis(build, directory):
    """Compare reusable C conversion APIs against Python's standard codecs."""
    records = []
    def record(mode, encoding, data, expected):
        records.append(f'{mode} {encoding} {data.hex() or "_"} {"-" if expected is None else expected.hex() or "_"}\n')
    for encoding, codec in [(1, 'cp932'), (2, 'euc_jp')]:
        candidates = [bytes([a]) for a in range(256)]
        if encoding == 1:
            candidates += [bytes([a, b]) for a in list(range(0x81, 0xa0)) + list(range(0xe0, 0xfd)) for b in range(256)]
        else:
            candidates += [bytes([a, b]) for a in [0x8e] + list(range(0xa1, 0xff)) for b in range(256)]
            candidates += [bytes([0x8f, a, b]) for a in range(0xa1, 0xff) for b in range(0xa1, 0xff)]
        characters = {'\0', '¥', '‾', '〜', '∥', '−', '😀'}
        for data in candidates:
            try:
                decoded = data.decode(codec)
            except UnicodeDecodeError:
                decoded = None
            record('D', encoding, data, decoded.encode() if decoded is not None else None)
            if decoded is not None:
                characters.update(decoded)
        for character in sorted(characters):
            try:
                expected = character.encode(codec)
            except UnicodeEncodeError:
                expected = None
            record('E', encoding, character.encode(), expected)
        record('T', encoding, '日本語'.encode(codec), bytes([encoding]))
    invalid = [b'\xc0\x80', b'\xed\xa0\x80', b'\xf4\x90\x80\x80', b'\xe3\x81', b'\x80', b'\xf8\x80\x80\x80']
    for data in invalid:
        record('D', 0, data, None)
        record('E', 1, data, None)
    for data in [b'', b'a\0b', '日本語😀'.encode(), chr(0x10ffff).encode()]:
        record('D', 0, data, data)
        record('E', 0, data, data)
        record('T', 0, data, b'\0')
    record('R', 0, b'a\xffz', 'a�z'.encode())
    # ISO-2022-JP (3): Python's iso2022_jp_ext, else the CP932 NEC rows.
    nec = {}
    for lead in list(range(0x81, 0xa0)) + list(range(0xe0, 0xf0)):
        for trail in range(0x40, 0xfd):
            try:
                nec.setdefault(bytes([lead, trail]).decode('cp932'), bytes([lead, trail]))
            except UnicodeDecodeError:
                pass
    def iso_expected(ch):
        if ch in '\x1b\x0e\x0f':
            return None  # would read back as a shift
        try:
            return ch.encode('iso2022_jp_ext')
        except UnicodeEncodeError:
            pass
        try:
            sjis = ch.encode('cp932')
        except UnicodeEncodeError:
            return None
        if len(sjis) != 2:
            return None
        if sjis[0] >= 0xf0:
            sjis = nec.get(ch)
            if sjis is None:
                return None
        return b'\x1b$B' + bytes(jis_from_sjis(sjis)) + b'\x1b(B'
    def iso_decoded(data):
        try:
            return data.decode('iso2022_jp_ext')
        except UnicodeDecodeError:
            j1, j2 = data[3], data[4]
            s1 = ((j1 + 1) >> 1) + (0x70 if j1 <= 0x5e else 0xb0)
            s2 = j2 + ((0x20 if j2 >= 0x60 else 0x1f) if j1 & 1 else 0x7e)
            return bytes([s1, s2]).decode('cp932')
    iso_count = 0
    for character in sorted(characters | {'¥', '‾', 'ｱ', '①', '髙', '～', '丂', '¦'}):
        expected = iso_expected(character)
        record('E', 3, character.encode(), expected)
        if expected is not None:
            record('D', 3, expected, iso_decoded(expected).encode())
            iso_count += 1
    require(iso_count > 13000, f'ISO-2022-JP cases {iso_count}')
    for data, expected in [(b'\x1b$@F|\x1b(Ba', '日a'), (b'\x1b&@\x1b$BF|', '日'),
                           (b'\x0e1\x0fa', 'ｱa'), (b'\x1b(J\\~', '¥‾'), (b'\x1b$(QF|', '日'),
                           (b'\x1b$BF|\nK\\', '日\n本'), (b'\x1b$B \x1b(B', ' '), (b'', '')]:
        record('D', 3, data, expected.encode())
    for data in [b'\x1b$Z', b'\x1b$BF', b'\x1b$', b'\xa4', b'\x1b(I\x60']:
        record('D', 3, data, None)
    record('R', 3, b'\x1b$ZA', '\ufffd$ZA'.encode())
    record('E', 3, '日\n'.encode(), b'\x1b$BF|\x1b(B\n')
    record('E', 3, '😀'.encode(), None)
    # UTF-16 (4 = LE, 5 = BE): no BOM written, BOM kept as U+FEFF when read.
    for text16 in ['', 'a\0b', '日本語 ｱ', '😀\U0010ffff', '\ufeffA']:
        for encoding, codec in [(4, 'utf-16-le'), (5, 'utf-16-be')]:
            record('D', encoding, text16.encode(codec), text16.encode())
            record('E', encoding, text16.encode(), text16.encode(codec))
    for data in [b'\x00\xd8', b'\x00\xdcA\x00', b'\x3d\xd8\x41\x00', b'A\x00B']:
        record('D', 4, data, None)
    record('R', 4, b'A\x00\x00\xdcB\x00', 'A\ufffdB'.encode())
    record('R', 5, b'\x00A\xdc\x00\x00B\x00', 'A\ufffdB\ufffd'.encode())
    # Detection of every encoding (A) and BOM lengths (B).
    for data, encoding in [(b'\xef\xbb\xbfabc', 0), (b'\xff\xfeA\x00', 4), (b'\xfe\xff\x00A', 5),
                           (b'abc', 0), (iso2022('日本語 ｱ'), 3), (b'\x1b(Jabc', 3), (b'\x1b$(D0!', 3),
                           (b'\x1b$B\xa4', 1), ('日本語'.encode('cp932'), 1), ('日本語'.encode('euc_jp'), 2),
                           ('日本語'.encode(), 0), (b'\x1bxyz', 0), (b'', 0)]:
        record('A', encoding, data, b'\0')
    for data, encoding, length in [(b'\xef\xbb\xbf', 0, 3), (b'\xef\xbb', 0, 0), (b'\xff\xfe', 4, 2),
                                   (b'\xfe\xff', 5, 2), (b'\xff\xfe', 5, 0), (b'\xef\xbb\xbf', 1, 0)]:
        record('B', encoding, data, bytes([length]))
    cases = directory / 'jis-cases.txt'
    cases.write_text(''.join(records))
    source = directory / 'jis-check.c'
    source.write_text(r"""
#include "jis.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t unhex(const char *text, unsigned char *bytes) {
    size_t count=0;
    if(!strcmp(text,"_")) return 0;
    while(*text) {
        unsigned value;
        if(sscanf(text,"%2x",&value)!=1) exit(3);
        bytes[count++]=(unsigned char)value; text+=2;
    }
    return count;
}
int main(int argc,char **argv) {
    if(argc!=2) return 3;
    FILE *file=fopen(argv[1],"r"); if(!file) return 3;
    char mode,input_hex[512],expected_hex[512]; int encoding;
    size_t line=0;
    while(fscanf(file," %c %d %511s %511s",&mode,&encoding,input_hex,expected_hex)==4) {
        ++line;
        unsigned char input[256],expected[256],*output=NULL;
        size_t n=unhex(input_hex,input),want=0,out_n=0;
        int fail=!strcmp(expected_hex,"-");
        if(!fail) want=unhex(expected_hex,expected);
        if(mode=='T') {
            if(jis_detect(input,n)!=(JisEncoding)encoding) { fprintf(stderr,"detection case %zu\n",line); return 1; }
            continue;
        }
        if(mode=='A') {
            if(jis_detect_all(input,n)!=(JisEncoding)encoding) { fprintf(stderr,"detect_all case %zu: %s\n",line,input_hex); return 1; }
            continue;
        }
        if(mode=='B') {
            if(jis_bom_length(input,n,(JisEncoding)encoding)!=expected[0]) { fprintf(stderr,"BOM case %zu\n",line); return 1; }
            continue;
        }
        int result;
        if(mode=='E') result=jis_from_utf8((const char *)input,n,(JisEncoding)encoding,&output,&out_n);
        else {
            char *text=NULL;
            result=jis_to_utf8(input,n,(JisEncoding)encoding,mode=='R',&text,&out_n);
            output=(unsigned char *)text;
        }
        if((fail && (result!=-1 || output || out_n)) ||
           (!fail && (result || !output || out_n!=want || memcmp(output,expected,want) || output[out_n]))) {
            fprintf(stderr,"jis case %zu: %c %d %s -> %s\n",line,mode,encoding,input_hex,expected_hex);
            free(output); fclose(file); return 1;
        }
        free(output);
    }
    fclose(file); return 0;
}
""")
    executable = directory / 'jis-check'
    command = shlex.split(os.environ.get('CC', 'cc'))
    command += ['-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'src'), '-I' + str(build), str(source),
                str(ROOT / 'src' / 'jis.c'), '-o', str(executable)]
    env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(build / 'tmp'))
    subprocess.run(command, check=True, env=env)
    subprocess.run([str(executable), str(cases)], check=True, env=env)
    print(f'  jis API: {len(records)} codec cases including UTF-8 boundaries/replacement/embedded NUL, ISO-2022-JP, UTF-16, detection')


def check_number_widths(build):
    directory = build / 'viewer-number-widths'
    directory.mkdir(exist_ok=True)
    exe = build / ('fdtp-preview' if sys.platform == 'darwin' else 'fdtp')
    env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(build / 'tmp'))
    for count, digits in [(9, 4), (9999, 4), (10000, 5), (123456, 6), (1, 7)]:
        # The long, newline-free case exceeds the bounded exact-count pass.
        content = b'X\n' * count if count != 1 else b'X' * (4 * 1024 * 1024 + 1)
        name = f'lines-{count}.txt'
        (directory / name).write_bytes(content)
        script = directory / 'width.keys'
        script.write_text(f'C-s "{name}" ESC v SNAP > SNAP < SNAP "#" SNAP "#" SNAP\n')
        prefix = directory / f'width-{count}'
        command = [str(exe)] + ([] if sys.platform == 'darwin' else ['--headless'])
        subprocess.run(command + ['--script', str(script), '--out', str(prefix), str(directory)],
                       check=True, env=env, timeout=60)
        snaps = [snapshot(p) for p in sorted(directory.glob(f'width-{count}-*.cells'))]
        require(len(snaps) == 5, f'{count}: width snapshots')
        for snap in [snaps[0], snaps[1], snaps[2], snaps[4]]:
            check_rules(snap, [digits + 1], f'{count}: stable {digits}-digit gutter')
        require(body(snaps[0], 1).startswith('X'), f'{count}: body starts after gutter')
        if count != 1:
            require(text(snaps[1], min(count, LAST), 1, 1 + digits).strip() == str(count),
                    f'{count}: full last line number')
        else:
            require(status(snaps[1]).rstrip().endswith('/1') or '/1 ' in status(snaps[1]), 'overestimate: exact count at EOF')
            require(len(body(snaps[0], 1).rstrip(' ')) == 79 - (digits + 2),
                    'overestimate: wrapping follows reserved gutter')
        require(snaps[0]['rows'][TOP:LAST + 1] == snaps[4]['rows'][TOP:LAST + 1],
                f'{count}: wrap and gutter restored after toggle')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    build = args.build.resolve()
    require(build.is_dir(), f'build directory missing: {build}')
    check_number_widths(build)
    directory, original = fixture(build)
    snaps = run(build, directory)
    check_jis(build, directory)
    check_edges(snaps['viewers_edges'])
    check_text(snaps['viewers_text'])
    check_formats(snaps['viewers_formats'])
    check_sparse(snaps['viewers_sparse'], directory)
    check_hex(snaps['viewers_hex'], directory, original)
    check_codes(snaps['viewers_codes'])
    check_units(snaps['viewers_units'])
    require((directory / 'u3-edit.bin').read_bytes() == bytes([0xab, 1, 2, 0xcd]) + bytes(range(4, 16)), 'swapped edit saved bytes')
    print(f'T3 viewer checks passed: {sum(len(v) - 1 for v in snaps.values())} snapshots, encodings, sparse reads, confirmed writes and discard')


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError, subprocess.SubprocessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
