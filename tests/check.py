#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Portable T1 acceptance checks; all fixtures and generated files stay in BUILD."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BG, CYAN, WHITE, YELLOW, MAGENTA = range(5)


def screen_path(path, limit=76, home=None):
    """A path as the app shows it (cells_path): $HOME and below as ~, and
    when wider than limit columns, "…" and the end of the path."""
    import unicodedata
    width = lambda t: sum(2 if unicodedata.east_asian_width(ch) in 'WF' else 1 for ch in t)
    p = str(path)
    if home:
        h = str(home)
        while len(h) > 1 and h.endswith('/'):
            h = h[:-1]
        if len(h) > 1 and (p == h or p.startswith(h + '/')):
            p = '~' + p[len(h):]
    if width(p) <= limit:
        return p
    while p and width(p) > limit - 2:
        p = p[1:]
    return '…' + p


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def text(snap, row, start=0, end=80):
    return ''.join(chr(c[0]) for c in snap['rows'][row][start:end] if c[3] != 2)


def info(snap):
    return text(snap, 4, 7, 24).rstrip()


def marked(snap):
    return int(text(snap, 3, 52, 58).strip() or '0')


def page(snap):
    return text(snap, 1, 2, 13).rstrip()


ERROR_BOX = 6  # WIDGET_ERROR in widgets.h
HELP_BOX = 7  # WIDGET_HELP
PRESS_ANY_KEY = '何かキーを押してください'


def popup_rows(snap):
    """Inner rows of the floating boxes (revision 1 §7), top box last. The frame
    of a whole screen (a box without a shadow: the customizer, the tree;
    revision 1 item 27) is not a floating box."""
    rows = []
    for col, row, w, h, _rule, kind in snap.get('boxes', []):
        if not kind & 2:
            continue
        rows += [(y, text(snap, y, col + 1, col + w - 1).strip()) for y in range(row + 1, row + h - 1)]
    # An error box's fixed last line (widgets.c) is not part of the message.
    if snap.get('widget') == ERROR_BOX and rows and rows[-1][1] == PRESS_ANY_KEY:
        rows.pop()
    return rows


def popup_text(snap):
    """Box rows run together: a long message wraps from row to row."""
    return ''.join(t for _, t in popup_rows(snap))


def notice(snap):
    """Row 23 (notices, the stack) and the text of any box."""
    return ' '.join(t for t in [text(snap, 23).strip()] + [t for _, t in popup_rows(snap)] if t)


def check_snapshot(path):
    snap = json.loads(path.read_text())
    require(len(snap['rows']) == 25, f'{path}: row count')
    for y, row in enumerate(snap['rows']):
        require(len(row) == 80, f'{path}: row {y} width')
        for x, c in enumerate(row):
            require(len(c) == 4 and all(isinstance(v, int) for v in c), f'{path}: cell shape')
            code, fg, bg, part = c
            require(0 <= code <= 0x10ffff and not 0xd800 <= code <= 0xdfff, f'{path}: invalid codepoint')
            require(0 <= fg < 9 and 0 <= bg < 9 and part in (0, 1, 2), f'{path}: palette/part')
            if part == 1:
                require(x < 79 and row[x + 1] == [code, fg, bg, 2], f'{path}: broken wide glyph at {x},{y}')
            if part == 2:
                require(x > 0 and row[x - 1] == [code, fg, bg, 1], f'{path}: orphan wide glyph at {x},{y}')
    ppm = path.with_suffix('.ppm').read_bytes()
    header = b'P6\n1024 600\n255\n'
    require(ppm.startswith(header) and len(ppm) == len(header) + 1024 * 600 * 3, f'{path}: PPM header/size')
    pixels = memoryview(ppm)[len(header):]
    for y in (0, 23, 299, 599):
        line = pixels[y * 1024 * 3:(y + 1) * 1024 * 3]
        require(line[:96] == b'\x00\x00\x55' * 32 and line[-96:] == b'\x00\x00\x55' * 32, f'{path}: navy canvas margins')
    return snap


def create_fixture(directory):
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    fixture = directory / 'files'
    fixture.mkdir()
    for name in ('aardir', 'zdir'):
        (fixture / name).mkdir()
    (fixture / 'aardir' / 'child.txt').write_text('child\n')
    contents = {'.hidden': 5, 'long-file-name-for-shift.txt': 100, 'run.sh': 3, '日本語.dat': 7}
    contents.update({f'f{i:02d}.txt': i + 1 for i in range(40)})
    for index, (name, size) in enumerate(contents.items()):
        file = fixture / name
        file.write_bytes(b'x' * size)
        os.utime(file, (1700000000 + index, 1700000000 + index))
    (fixture / 'run.sh').chmod(0o755)
    (fixture / 'long-file-name-for-shift.txt').chmod(0o755)
    (fixture / 'sym').symlink_to('f01.txt')
    return fixture


def run_scripts(build, fixture, env, executable, headless):
    result = {}
    # Revision 1 §2 dropped the S T O keys (link target, type symbols, file
    # flags); the scripts get them back from ~/.fdtprc binds, which also shows
    # that a user's bind of a Shift+letter wins over the initial-letter jump.
    rc = build / 'check-fixture' / 't1.fdtprc'
    rc.write_text('bind S SYMLINK_MODE\nbind T FILETYPE_MODE\nbind O FILEFLG_MODE\n')
    env = dict(env, FDTP_RC_USER=str(rc))
    # T1's own scripts; the modules' scripts (fileops_*, viewers_*, image_* ...)
    # belong to their check_*.py.
    t1 = {'directories', 'edge_cases', 'external', 'filter_sort', 'marks', 'navigation', 'stack_windows'}
    for script in sorted(p for p in (ROOT / 'tests').glob('*.keys') if p.stem in t1):
        source = script.read_text()
        labels = re.findall(r'\bSNAP\s+#\s*([a-z0-9_]+)', source)
        require(len(labels) == len(set(labels)), f'{script}: duplicate snapshot labels')
        require(len(labels) == len(re.findall(r'\bSNAP\b', source)), f'{script}: every SNAP needs a label')
        script_file = build / 'check-fixture' / script.name
        # The placeholder is already inside a quoted key string.
        escaped = str(fixture).replace('\\', '\\\\').replace('"', '\\"')
        script_file.write_text(source.replace('@FIXTURE@', escaped))
        prefix = build / 'check-fixture' / script.stem
        subprocess.run([str(executable)] + (['--headless'] if headless else []) + ['--script', str(script_file), '--out', str(prefix), '--time', '1700000000', str(fixture)], check=True, cwd=ROOT.parent, env=env)
        files = sorted(prefix.parent.glob(prefix.name + '-*.cells'))
        require(len(files) == len(labels), f'{script}: expected {len(labels)} snapshots, got {len(files)}')
        result[script.stem] = {label: check_snapshot(prefix.with_name(f'{prefix.name}-{i:03d}.cells')) for i, label in enumerate(labels, 1)}
        print(f'  {script.name}: {len(labels)} snapshots')
    return result


def check_navigation(s):
    for label, name in [('initial', '..'), ('directory', 'aardir'), ('right', 'f13.txt'), ('page2', 'f30.txt'), ('page1', '..')]:
        require(info(s[label]) == name, f'navigation/{label}: Info was {info(s[label])!r}')
    require(page(s['initial']) == 'Page: 1/ 2' and page(s['page2']) == 'Page: 2/ 2', 'page navigation')
    require(info(s['last']) == '日本語.dat', 'last entry movement')
    require('rwx' in text(s['onecol'], 6), 'one-column mode attribute field')
    require('f00' in text(s['threecol'], 10), 'three-column mode entries')
    require('f13' in text(s['fivecol'], 6), 'five-column mode third slot')
    require(info(s['longname']).startswith('long-file-name-'), 'incremental prefix search')
    require(info(s['shifted']) == info(s['longname'])[1:] + 'r', 'name shift reflects Info')
    require(info(s['unshifted']) == info(s['longname']), 'name shift restored')
    prompt = s['quit_prompt']
    require('FDTP を終了しますか？ (Y/N)' in notice(prompt) and not prompt['quit'], 'quit confirmation')
    require(any(c[1] == YELLOW and c[0] != 32 for y, _ in popup_rows(prompt) for c in prompt['rows'][y]), 'confirmation yellow')
    require(not s['cancelled']['quit'] and not notice(s['cancelled']), 'cancel quit')
    require(s['quit']['quit'], 'accept quit')
    initial = s['initial']['rows']
    require(s['directory']['rows'][7][2][2] == CYAN and initial[7][2][1] == CYAN, 'directory cursor and directory color')
    require(initial[9][2][1] == MAGENTA and initial[10][2][1] == YELLOW, 'hidden/file colors')


def check_marks(s):
    expected = {'directory_unmarked': 0, 'one_mark': 1, 'space_unmark_move': 0, 'control_space': 1, 'all_marked': 45, 'inverted': 0, 'wildcard': 41, 'control_space_boundary': 40, 'space_next_page': 41}
    for label, count in expected.items():
        require(marked(s[label]) == count, f'marks/{label}: {marked(s[label])} != {count}')
    require(info(s['one_mark']) == 'f00.txt' and s['one_mark']['rows'][10][1][:3] == [ord('*'), YELLOW, BG], 'TAB marks without moving')
    require(info(s['space_unmark_move']) == 'f01.txt' and info(s['control_space']) == 'f02.txt', 'space cursor movement')
    require(text(s['all_marked'], 3, 64, 78).strip() == '937', 'marked byte sum')
    require(info(s['control_space_boundary']) == 'f29.txt' and page(s['control_space_boundary']) == 'Page: 1/ 2', 'C-SPC keeps current page')
    require(info(s['space_next_page']) == 'f30.txt' and page(s['space_next_page']) == 'Page: 2/ 2', 'SPC crosses page')


def check_directories(s, fixture):
    require('aardir' in text(s['child'], 3, 7, 51) and 'child' in text(s['child'], 7), 'enter directory')
    require(info(s['parent_cursor']) == 'aardir', 'parent restores child cursor')
    require(info(s['dot_path']) == '..' and info(s['previous_path']) == '..', 'dot and previous paths')
    require('移動履歴' in notice(s['directory_history']), 'path history choice')
    box = [t for _, t in popup_rows(s['completion'])]
    require(box[0].startswith('移動先') and box[1] == 'aardir/', f'cwd filename completion: {box!r}')
    require('aardir' in text(s['completed_child'], 3, 7, 51), 'completed path entered')
    require(text(s['filesystem_root'], 3, 7, 51).strip() == '/' and info(s['filesystem_root']) != '..', 'root command')
    require(info(s['returned']) == '..', 'absolute path + reread')
    expected_suffix = str(fixture)[-25:]
    require(expected_suffix in text(s['returned'], 3, 7, 51), 'Path shows target directory')


def check_stack_windows(s):
    require('f00.txt' in notice(s['pushed']) and info(s['pushed']) == 'f01.txt', 'push removes current item and shows stack')
    require(not notice(s['popped']) and info(s['popped']) == 'f00.txt', 'pop returns at cursor')
    require(info(s['popped_at_cursor']) == 'f00.txt' and not notice(s['popped_at_cursor']), 'bracket aliases pop at chosen cursor')
    require(s['two_windows']['separators'].bit_count() == 1, 'split separator')
    require(s['two_windows']['separators'] != s['widened']['separators'] and s['two_windows']['separators'] == s['narrowed']['separators'], 'window widen/narrow')
    require('aardir' in text(s['other_window_directory'], 3, 7, 51), 'next window has own directory')
    require('aardir' not in text(s['first_window_directory'], 3, 7, 51), 'first window directory retained')
    require(s['five_windows']['separators'].bit_count() == 4, 'five windows')
    require('窓は5個までです' in notice(s['window_limit']), 'window capacity notice')
    require(s['one_window']['separators'] == 0, 'close windows')
    # Revision 1 item 25: the help is a box over the filer (WIDGET_HELP).
    require(s['help']['depth'] == 1 and s['help']['widget'] == HELP_BOX and 'FDTP キー一覧' in ''.join(text(s['help'], y) for y in range(25)), 'help box')
    require(s['help_closed']['depth'] == 1 and s['help_closed']['widget'] == 0 and 'Path=' in text(s['help_closed'], 3), 'Esc closes the help')


def check_filter_sort(s):
    require('f09' in text(s['filter'], 16) and not text(s['filter'], 17).strip(), 'wildcard filter count')
    require(page(s['filter']) == 'Page: 1/ 1' and page(s['filter_cleared']) == 'Page: 1/ 2', 'clear filter')
    require('並べ替え' in notice(s['sort_prompt']) and '並べない' in notice(s['sort_prompt']), 'sort choices')
    order = [t for _, t in popup_rows(s['order_prompt'])]
    require(order == ['並び順', '昇順  降順'], f'sort order choices: {order!r}')
    require(info(s['size_descending']).startswith('long-file-name-'), 'descending size ordering')
    require('並べない' not in notice(s['unsorted_menu']), 'unsorted current state menu')
    require(info(s['link_target']) == 'f01.txt' and text(s['link_target'], 24, 0, 2) == 'ST', 'symlink target and flags')
    require(text(s['toggles'], 24, 0, 3) == 'STH' and '.hidden' not in ''.join(text(s['toggles'], y) for y in range(6, 23)), 'hidden toggle')
    require('この OS では使えません' in notice(s['unavailable']), 'unavailable file flags')
    require(s['unavailable']['widget'] == ERROR_BOX and s['unavailable']['boxes'], 'error notice in a box')
    require(not notice(s['error_box_takes_key']) and info(s['error_box_takes_key']) == '..', 'error box takes the key that closes it')
    require('ファイルを選んでください' in text(s['plain_notice'], 23) and not s['plain_notice']['boxes'], 'plain notice on row 23')
    require(not notice(s['notice_passes']) and info(s['notice_passes']) == 'aardir', 'notice passes DOWN key')
    search = [t for _, t in popup_rows(s['incremental_search'])]
    require(search == ['検索', 'f0'] and info(s['incremental_search']) == 'f09.txt', f'forward/reverse incremental search: {search!r}')
    require(not notice(s['search_closed']), 'close search')


def check_external(s, fixture):
    for label, snap in s.items():
        request = snap['external']
        require(request['pending'] and request['flags'] == 2, f'external/{label}: terminal request')
        require(request['cwd'] == str(fixture), f'external/{label}: directory copied')
        require(request['argv'] == ['test-editor', '--flag', 'two words', str(fixture / 'f00.txt')], f'external/{label}: editor words/path {request["argv"]!r}')


def check_edges(s):
    require('sym' in text(s['extension_order'], 10) and '日本語.dat' in text(s['extension_order'], 11), 'extension sort')
    require('long-file-name-' in text(s['date_order'], 10), 'date sort')
    require('sym' in text(s['length_order'], 9) and 'run.sh' in text(s['length_order'], 10), 'length sort')
    require('long-file-name-' in text(s['length_descending'], 9), 'length descending')
    require(notice(s['stack_full']) == 'スタックは5個までです', 'stack capacity')
    require(all(f'f{i:02d}.txt' in notice(s['stack_names']) for i in range(5)), 'all stack names')
    require(not notice(s['stack_empty']), 'stack empty after five pops')
    require('run.sh*' in ''.join(text(s['executable_symbol'], y) for y in range(6, 23)), 'executable symbol')
    require(any(c[0] == ord('*') for row in s['clipped_symbol']['rows'][6:23] for c in row[2:42]), 'symbol survives clipped name')
    require(notice(s['missing_directory']) == 'ディレクトリを開けません' and s['missing_directory']['widget'] == ERROR_BOX, 'bad path notice')
    require(info(s['missing_directory']) == info(s['cancelled_path']), 'cancel path preserves cursor')
    require(info(s['parent_filter_reset']) == 'aardir' and page(s['parent_filter_reset']) == 'Page: 1/ 3', 'directory switch clears filter and restores parent cursor')
    require(s['parent_filter_reset']['rows'] == s['waited']['rows'], 'WAIT preserves screen within the minute')


def run_unit(build, env):
    # The Makefile passes its object list and the modules' libraries (mk/*.mk).
    common = env.get('FDTP_COMMON', 'font24 cells canvas raster dirlist fdscreen keys widgets commands app filer headless fileops textview hexview archive config launcher imageview pdfview sheetview docview slideview noveledit sysinfo').split()
    command = shlex.split(env.get('CC', 'cc')) + ['-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'), '-I' + str(build), str(ROOT / 'tests' / 'unit.c')]
    command += [str(build / (name + '.o')) for name in common]
    command += ['-o', str(build / 'test-unit')] + shlex.split(env.get('FDTP_MODULE_LIBS', ''))
    subprocess.run(command, check=True, cwd=ROOT.parent, env=env)
    subprocess.run([str(build / 'test-unit'), str(build / 'tmp'), str(build / 'check-fixture' / 'files')], check=True, cwd=ROOT.parent, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--executable', type=Path)
    parser.add_argument('--headless', action='store_true')
    args = parser.parse_args()
    build = args.build.resolve()
    (build / 'tmp').mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(build / 'tmp'), TZ='UTC', VISUAL='test-editor --flag "two words"', EDITOR='unused-editor', FDTP_RC_SYSTEM='', FDTP_RC_USER='')
    fixture = create_fixture(build / 'check-fixture')
    run_unit(build, env)
    executable = args.executable.resolve() if args.executable else build / 'fdtp-preview'
    snapshots = run_scripts(build, fixture, env, executable, args.headless)
    check_navigation(snapshots['navigation'])
    check_marks(snapshots['marks'])
    check_directories(snapshots['directories'], fixture)
    check_stack_windows(snapshots['stack_windows'])
    check_filter_sort(snapshots['filter_sort'])
    check_external(snapshots['external'], fixture)
    check_edges(snapshots['edge_cases'])
    # stubs.keys checked T1's placeholders; the modules now replace them and
    # their own check_*.py test them.
    for label, snap in snapshots.get('stubs', {}).items():
        require(notice(snap) == '未実装です', f'stubs/{label}: {notice(snap)!r}')
        require(snap['depth'] == 1 and not snap['external']['pending'], f'stubs/{label}: no screen/external transition')
    print(f'T1 checks passed: {sum(len(s) for s in snapshots.values())} snapshots and public API unit checks')


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError, subprocess.CalledProcessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
