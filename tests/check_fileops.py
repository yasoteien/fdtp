#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""T2 acceptance: isolated headless scripts, screen checks and filesystem assertions.

Run from the repository root: python3 -B tests/check_fileops.py --build build
Every generated script, snapshot and fixture stays under BUILD/fileops-fixture.
External commands are inspected as requests; this check never launches a shell.
"""
import argparse
import json
import os
import re
from pathlib import Path
import shutil
import stat
import subprocess
import sys

from check import check_snapshot, notice, require, text

ROOT = Path(__file__).resolve().parents[1]


def quote(value):
    return json.dumps(str(value), ensure_ascii=False)


def select(name):
    return 'C-s ' + quote(name) + ' ESC '


def enter(value):
    return 'C-u ' + quote(value) + ' RET '


class Acceptance:
    def __init__(self, build):
        self.build = build
        self.base = build / 'fileops-fixture'
        if self.base.exists():
            shutil.rmtree(self.base)
        self.base.mkdir(parents=True)
        self.env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(build / 'tmp'), TZ='UTC', SHELL='/bin/sh')
        self.executable = build / ('fdtp-preview' if sys.platform == 'darwin' else 'fdtp')
        self.count = 0
        self.snapshots = 0

    def case(self, name):
        home = self.base / name
        home.mkdir()
        for child in ('src', 'dst', 'old'):
            (home / child).mkdir()
        return home, home / 'src', home / 'dst'

    def run(self, home, keys, settle_keys=True):
        script = home / 'run.keys'
        # Snapshots are always labelled, including generated scripts.
        # Each WAIT yields to app_tick. Zero seconds keeps the fixed clock while
        # allowing bounded asynchronous work to settle before the next key.
        settle = ' ' + 'WAIT:0 ' * 128
        tokens = re.compile(r'"(?:[^"\\]|\\.)*"|#[^\n]*|\bRET\b|\by\b')
        if settle_keys:
            keys = tokens.sub(lambda match: match[0] + settle if match[0] in ('RET', 'y') else match[0], keys)
        script.write_text(keys + '\n' + settle + '\nSNAP # result\n')
        prefix = home / 'screen'
        for old in home.glob('screen-*'):
            old.unlink()
        subprocess.run([str(self.executable), '--headless', '--script', str(script),
                        '--out', str(prefix), '--time', '1700000000', str(home / 'src')],
                       check=True, cwd=ROOT.parent, env=self.env, stdout=subprocess.DEVNULL)
        result = [check_snapshot(p) for p in sorted(home.glob('screen-*.cells'))]
        self.snapshots += len(result)
        self.count += 1
        require(result, f'{home.name}: no snapshots')
        print(f'  {home.name}: {len(result)} snapshots')
        return result

    def file(self, path, data='new contents\n', mode=0o640, mtime=1700000123):
        path.write_text(data)
        path.chmod(mode)
        os.utime(path, (mtime, mtime))
        return path

    def transfer(self):
        for command in ('c', 'm'):
            home, src, dst = self.case('copy' if command == 'c' else 'move')
            source = self.file(src / '日本語.txt', mode=0o751)
            os.utime(source, ns=(1700000123123456789, 1700000123123456789))
            before = source.stat()
            self.run(home, 'DOWN ' + command + ' ' + enter(dst))
            target = dst / source.name
            require(target.read_text() == 'new contents\n', f'{command}: Japanese filename/data')
            require(stat.S_IMODE(target.stat().st_mode) == 0o751, f'{command}: permissions')
            require(target.stat().st_mtime_ns == before.st_mtime_ns, f'{command}: modification time')
            require(source.exists() == (command == 'c'), f'{command}: source lifetime')
            if command == 'm':
                require(target.stat().st_ino == before.st_ino, 'same-device move must use rename')

        home, src, dst = self.case('copy_recursive_links')
        tree = src / 'bundle'
        (tree / 'deep').mkdir(parents=True)
        self.file(tree / 'deep' / '日本語.txt', 'deep data\n', 0o444)
        (tree / 'relative-link').symlink_to('deep/日本語.txt')
        (tree / 'broken-link').symlink_to('missing')
        self.run(home, select('bundle') + 'c ' + enter(dst))
        require((dst / 'bundle/deep/日本語.txt').read_text() == 'deep data\n', 'recursive data')
        require(stat.S_IMODE((dst / 'bundle/deep/日本語.txt').stat().st_mode) == 0o444, 'recursive readonly mode')
        require(os.readlink(dst / 'bundle/relative-link') == 'deep/日本語.txt', 'copy symlink unchanged')
        require(os.readlink(dst / 'bundle/broken-link') == 'missing', 'copy dangling symlink')

        home, src, dst = self.case('marked_copy')
        for name in ('alpha', 'beta', 'untouched'):
            self.file(src / name, name)
        self.run(home, select('alpha') + 'TAB ' + select('beta') + 'TAB c ' + enter(dst))
        require(sorted(p.name for p in dst.iterdir()) == ['alpha', 'beta'], 'marked selection overrides cursor')
        require(len(list(src.iterdir())) == 3, 'copy keeps all sources')

        for choice, rights in (('newer', 0), ('rename', 1), ('all', 2), ('skip', 3), ('relocate', 4)):
            for newer in ((True, False) if choice == 'newer' else (True,)):
                home, src, dst = self.case('conflict_' + choice + ('_new' if newer else '_old'))
                self.file(src / 'alpha', 'source', mtime=1700000300 if newer else 1700000000)
                self.file(dst / 'alpha', 'original', mtime=1700000100)
                keys = select('alpha') + 'c ' + enter(dst) + 'RIGHT ' * rights + 'RET '
                if choice == 'rename':
                    keys += enter('alternate')
                if choice == 'relocate':
                    keys += enter(home / 'old')
                self.run(home, keys)
                copied = choice not in ('skip', 'rename') and (choice != 'newer' or newer)
                require((dst / 'alpha').read_text() == ('source' if copied else 'original'), f'{choice}: conflict result')
                if choice == 'rename':
                    require((dst / 'alternate').read_text() == 'source', 'rename conflict keeps original')
                if choice == 'relocate':
                    require((home / 'old/alpha').read_text() == 'original', 'relocate preserves old destination')
                require((src / 'alpha').read_text() == 'source', 'conflict copy preserves source')

        home, src, dst = self.case('conflict_all_marked')
        for name in ('alpha', 'beta'):
            self.file(src / name, name)
            self.file(dst / name, 'old-' + name)
        self.run(home, select('alpha') + 'TAB ' + select('beta') + 'TAB c ' +
                 enter(dst) + 'RIGHT RIGHT RET ')
        require(all((dst / name).read_text() == name for name in ('alpha', 'beta')),
                'overwrite-all applies to subsequent marked conflicts')
        home, src, dst = self.case('move_conflict_skip')
        self.file(src / 'alpha', 'source')
        self.file(dst / 'alpha', 'destination')
        self.run(home, select('alpha') + 'm ' + enter(dst) + 'RIGHT RIGHT RIGHT RET ')
        require((src / 'alpha').read_text() == 'source' and (dst / 'alpha').read_text() == 'destination',
                'skipped move preserves both files')
        home, src, dst = self.case('copy_same_file')
        self.file(src / 'alpha', 'safe')
        self.run(home, select('alpha') + 'c ' + enter(src) + 'RIGHT RIGHT RET ')
        require((src / 'alpha').read_text() == 'safe', 'same-file copy cannot truncate source')

        if os.geteuid() != 0:
            home, src, dst = self.case('copy_unreadable_keeps_destination')
            self.file(src / 'alpha', 'unreadable', mode=0o000)
            self.file(dst / 'alpha', 'preserved destination')
            self.run(home, select('alpha') + 'c ' + enter(dst) + 'RIGHT RIGHT RET ')
            require((dst / 'alpha').read_text() == 'preserved destination', 'failed copy preserves existing destination')
            (src / 'alpha').chmod(0o600)
            require((src / 'alpha').read_text() == 'unreadable', 'failed copy preserves source')

        home, src, dst = self.case('reject_self_descendant')
        (src / 'bundle').mkdir()
        self.file(src / 'bundle/data', 'safe')
        self.run(home, select('bundle') + 'c ' + enter(src / 'bundle'))
        require(sorted(p.name for p in (src / 'bundle').iterdir()) == ['data'], 'copy into descendant rejected')
        require((src / 'bundle/data').read_text() == 'safe', 'descendant rejection preserves contents')

    def remove_and_names(self):
        for accept in (False, True):
            home, src, _ = self.case('delete_' + str(accept).lower())
            self.file(src / 'readonly', mode=0o444)
            snapshots = self.run(home, select('readonly') + 'd SNAP # confirm\n' +
                                 ('y SNAP # readonly\ny ' if accept else 'n '))
            require('1' in notice(snapshots[0]) and 'Y/N' in notice(snapshots[0]), 'delete count confirmation')
            if accept:
                require('Y/N' in notice(snapshots[1]), 'readonly file separately confirmed')
            require((src / 'readonly').exists() != accept, 'delete/cancel readonly result')
        home, src, _ = self.case('readonly_delete_refused')
        self.file(src / 'readonly', mode=0o444)
        self.run(home, select('readonly') + 'd y n ')
        require((src / 'readonly').exists(), 'declining readonly confirmation preserves file')
        # Revision 1 item 36: d removes a directory with what it holds, after
        # a confirmation that counts the files inside; n keeps it.
        home, src, _ = self.case('delete_directory')
        (src / 'directory/sub').mkdir(parents=True)
        self.file(src / 'directory/keep'); self.file(src / 'directory/sub/deep')
        snapshots = self.run(home, select('directory') + 'd SNAP # confirm\nn ')
        require('2' in notice(snapshots[0]) and 'ディレクトリ' in notice(snapshots[0]), 'd counts the files in a directory')
        require((src / 'directory/keep').exists(), 'declining d keeps the directory')
        self.run(home, select('directory') + 'd y ')
        require(not (src / 'directory').exists(), 'd removes directories')
        home, src, _ = self.case('remove_directory')
        (src / 'bundle/deep').mkdir(parents=True)
        self.file(src / 'bundle/deep/data')
        snapshots = self.run(home, select('bundle') + 'C-d SNAP # confirm\ny ')
        require('1' in notice(snapshots[0]) and 'Y/N' in notice(snapshots[0]), 'recursive delete count')
        require(not (src / 'bundle').exists(), 'D removes recursive contents')
        home, src, _ = self.case('remove_directory_link')
        (home / 'outside').mkdir()
        self.file(home / 'outside/keep')
        (src / 'linkdir').symlink_to(home / 'outside', target_is_directory=True)
        self.run(home, select('linkdir') + 'C-d y ')
        require(not (src / 'linkdir').is_symlink() and (home / 'outside/keep').exists(), 'D unlinks directory symlink only')

        home, src, _ = self.case('mkdir_rename')
        self.file(src / 'alpha', 'original')
        self.file(src / 'collision', 'keep')
        self.run(home, 'k ' + enter('deep/nested') + select('alpha') + 'r ' + enter('deep/nested/renamed'))
        require((src / 'deep/nested/renamed').read_text() == 'original', 'recursive mkdir and rename with slash')
        require(not (src / 'alpha').exists(), 'rename removes old name')
        self.run(home, select('collision') + 'r ' + enter('deep/nested/renamed'))
        require((src / 'collision').read_text() == 'keep', 'rename collision preserves source')
        require((src / 'deep/nested/renamed').read_text() == 'original', 'rename collision preserves target')
        self.run(home, 'k ' + enter(src / 'absolute/child'))
        require((src / 'absolute/child').is_dir(), 'absolute recursive mkdir')

    def external_and_info(self):
        home, src, _ = self.case('external_requests')
        self.file(src / 'run.sh', '#!/bin/sh\necho safe\n', 0o755)
        snapshots = self.run(home, select('run.sh') + 'x SNAP # initial\n' +
                             enter('printf explicit-command') + 'SNAP # execute\n' +
                             'S-F6 ' + enter('printf shell-command') + 'SNAP # shell\n' +
                             'S-F6 RET SNAP # interactive\n')
        require('run.sh' in notice(snapshots[0]), 'execute initial filename')
        for snap, command in ((snapshots[1], 'printf explicit-command'),
                              (snapshots[2], 'printf shell-command')):
            request = snap['external']
            require(request['pending'] and request['cwd'] == str(src), 'external cwd and pending request')
            require(request['argv'] == ['/bin/sh', '-c', command], 'command through /bin/sh -c')
        require(snapshots[3]['external']['pending'] and
                snapshots[3]['external']['argv'] == ['/bin/sh'], 'empty h requests SHELL')
        require((src / 'run.sh').read_text() == '#!/bin/sh\necho safe\n', 'headless external request preserves file')
        home, src, _ = self.case('filesystem_info')
        snapshots = self.run(home, 'S-F2 RET SNAP # info\nESC SNAP # returned\nw ')
        require(snapshots[0]['widget'] == 5 and snapshots[0]['boxes'] and snapshots[1]['depth'] == 1 and not snapshots[1]['boxes'], 'filesystem information box (Shift+F2) closes')
        rendered = '\n'.join(text(snapshots[0], row) for row in range(25))
        require(any(word in rendered for word in ('容量', '総', 'ブロック')), 'filesystem capacity information')
        filesystem = os.statvfs(src)
        require(str(filesystem.f_blocks * (filesystem.f_frsize or filesystem.f_bsize)) in rendered,
                'filesystem total capacity agrees with statvfs')
        require('Linux' in notice(snapshots[-1]) and '使えません' in notice(snapshots[-1]), 'w unavailable notice')

    def attributes(self):
        home, src, _ = self.case('attributes_mode_date')
        self.file(src / 'alpha', mode=0o640)
        snapshots = self.run(home, select('alpha') + 'a SNAP # editor\nSPC RET ')
        require(any('属性' in text(snapshots[0], row) for row in range(10, 23)), 'attribute editor lower half')
        # Item 27: a floating box with the frame and the shadow, rows 10..21.
        require([b[:4] + [b[5]] for b in snapshots[0]['boxes']] == [[1, 10, 77, 12, 3]], f'attribute box: {snapshots[0]["boxes"]}')
        require(stat.S_IMODE((src / 'alpha').stat().st_mode) == 0o240, 'attribute mode bit toggle')
        self.run(home, select('alpha') + 'a d "240229" t "1234" RET ')
        import datetime
        expected = datetime.datetime(2024, 2, 29, 12, 34, 23, tzinfo=datetime.timezone.utc).timestamp()
        require((src / 'alpha').stat().st_mtime == expected, 'attribute date and time')
        self.run(home, select('alpha') + 'a SPC ESC ')
        require(stat.S_IMODE((src / 'alpha').stat().st_mode) == 0o240, 'attribute cancel preserves mode')
        home, src, _ = self.case('attributes_marked_mask')
        self.file(src / 'alpha', mode=0o200)
        self.file(src / 'beta', mode=0o640)
        self.run(home, select('alpha') + 'TAB ' + select('beta') + 'TAB a RET SPC RET ')
        require(stat.S_IMODE((src / 'alpha').stat().st_mode) == 0o600 and
                stat.S_IMODE((src / 'beta').stat().st_mode) == 0o640, 'marked mode keeps masked bits')
        home, src, _ = self.case('attributes_zero_mode')
        self.file(src / 'alpha', mode=0o000)
        self.run(home, select('alpha') + 'a SPC RET ')
        require(stat.S_IMODE((src / 'alpha').stat().st_mode) == 0o400, 'owner can restore mode 000 file')
        home, src, _ = self.case('attributes_recursive_x')
        (src / 'bundle/child').mkdir(parents=True)
        (src / 'bundle').chmod(0o700)
        (src / 'bundle/child').chmod(0o700)
        self.file(src / 'bundle/plain', mode=0o600)
        self.file(src / 'bundle/program', mode=0o700)
        self.run(home, select('bundle') + 'C-a RIGHT RIGHT SPC SPC RET ')
        require(stat.S_IMODE((src / 'bundle/plain').stat().st_mode) == 0o600, 'recursive X keeps nonexecutable file')
        require(stat.S_IMODE((src / 'bundle/program').stat().st_mode) == 0o700, 'recursive X keeps executable file')
        require(stat.S_IMODE((src / 'bundle/child').stat().st_mode) == 0o700, 'recursive X directory mode')

    def tree_and_find(self):
        # Revision 1 §2: L C M are gone; F8 in the path/destination input opens
        # the tree, whose RET puts the directory in the field (RET accepts).
        for command in ('t', 'l F8'):
            home, src, _ = self.case('tree_' + ('t' if command == 't' else 'L'))
            (src / 'child/grandchild').mkdir(parents=True)
            snapshots = self.run(home, command + ' SNAP # tree\nRIGHT DOWN RET ' + ('RET ' if command != 't' else ''))
            require(snapshots[0]['depth'] > 1 and 'ツリー' in text(snapshots[0], 0) and text(snapshots[0], 21).strip().endswith('/src'),
                    'tree starts with current path expanded')
            require('child' in text(snapshots[-1], 3), 'tree selected directory entered')
            require(any(ch in text(snapshots[0], row) for row in range(1, 20) for ch in '├└'), 'tree guide lines')
        for command in ('c', 'm'):
            home, src, _ = self.case('tree_transfer_' + command)
            (src / 'destination').mkdir()
            self.file(src / 'alpha', 'tree data')
            self.run(home, select('alpha') + command + ' F8 RIGHT DOWN RET RET ')
            require((src / 'destination/alpha').read_text() == 'tree data', 'tree transfer destination')
            require((src / 'alpha').exists() == (command == 'c'), 'tree copy/move source')
        home, src, _ = self.case('find_directory')
        (src / 'branch/target-dir').mkdir(parents=True)
        snapshots = self.run(home, 'C-f ' + enter('target-*') + 'SNAP # match\ny ')
        require('Y/N' in notice(snapshots[0]), 'directory find confirmation')
        require('target-dir' in text(snapshots[-1], 3), 'directory find enters accepted match')
        home, src, _ = self.case('find_cancel')
        (src / 'target-dir').mkdir()
        snapshots = self.run(home, 'C-f ' + enter('target-*') + 'ESC ')
        require(snapshots[-1]['depth'] == 1 and 'target-dir' not in text(snapshots[-1], 3), 'find ESC preserves directory')

    def live_progress(self):
        home, src, dst = self.case('live_copy_progress')
        for index in range(10):
            self.file(src / ('item%02d' % index), str(index))
        snapshots = self.run(home, 'HOME c ' + enter(dst) + 'SNAP # progress\n', settle_keys=False)
        require(snapshots[0]['depth'] > 1 and 'コピー中' in notice(snapshots[0]), 'copy exposes live progress before completion')
        require(len(list(dst.iterdir())) == 10, 'tick-driven copy eventually completes')

    def spaced_names(self):
        # Japanese names with spaces, recursive move, rename, delete and D.
        home, src, dst = self.case('spaced_names')
        tree = src / '写真 2026'
        (tree / '旅行 夏').mkdir(parents=True)
        self.file(tree / '旅行 夏' / 'メモ 1.txt', 'memo\n', 0o600)
        (tree / 'link to memo').symlink_to('旅行 夏/メモ 1.txt')
        # The filer's incremental search is ASCII only until stage E: DOWN picks the only entry.
        self.run(home, 'DOWN m ' + enter(dst))
        require(not tree.exists(), 'spaced directory moved away')
        require((dst / '写真 2026/旅行 夏/メモ 1.txt').read_text() == 'memo\n', 'spaced recursive move data')
        require(os.readlink(dst / '写真 2026/link to memo') == '旅行 夏/メモ 1.txt', 'move keeps symlink')
        self.file(src / 'a b.txt', 'spaced')
        self.file(src / "it's.txt", 'quote')
        self.run(home, select('a b.txt') + 'r ' + enter('c d/e f.txt'))
        require(not (src / 'c d/e f.txt').exists() and (src / 'a b.txt').exists(),
                'rename into a missing directory is refused')
        self.run(home, 'k ' + enter('c d') + select('a b.txt') + 'r ' + enter('c d/e f.txt'))
        require((src / 'c d/e f.txt').read_text() == 'spaced', 'rename with spaces into directory')
        self.run(home, select("it's.txt") + 'd y ')
        require(not (src / "it's.txt").exists(), 'delete name with quote')
        snapshots = self.run(home, select("c d") + 'C-d SNAP # confirm\ny ')
        require('c d' in notice(snapshots[0]) and '1 個' in notice(snapshots[0]), 'D names the directory and count')
        require(not (src / 'c d').exists(), 'D removes spaced directory')
        # x quotes names that need it; executable names get ./ and arguments follow.
        self.file(src / 'run me.sh', '#!/bin/sh\n', 0o755)
        self.file(src / 'plain text', 'x')
        snapshots = self.run(home, select('run me.sh') + 'x SNAP # exec\n' + '" now" RET SNAP # run\n' +
                             select('plain text') + 'x "cat" RET SNAP # cat\n' + 'S-F6 RET SNAP # shell\n')
        require(snapshots[0]['rows'] and "./'run me.sh'" in notice(snapshots[0]), 'x initial executable text')
        require(snapshots[1]['external']['argv'] == ['/bin/sh', '-c', "./'run me.sh' now"] and
                snapshots[1]['external']['flags'] == 3, 'x runs through sh -c and pauses')
        require(snapshots[2]['external']['argv'] == ['/bin/sh', '-c', "cat 'plain text'"], 'x puts command before a plain file')
        require(snapshots[3]['external']['flags'] == 2, 'interactive h does not pause')

    def smoke_templates(self):
        for template in sorted((ROOT / 'tests').glob('fileops_*.keys')):
            home, src, _ = self.case(template.stem)
            self.file(src / 'alpha', 'unchanged')
            self.run(home, template.read_text())
            require((src / 'alpha').read_text() == 'unchanged' and len(list(src.iterdir())) == 1,
                    f'{template.name}: cancellation preserves files')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    build = args.build.resolve()
    require(build.is_relative_to(ROOT / 'build'), 'build and all generated fixtures must be under build/')
    (build / 'tmp').mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(build / 'tmp'))
    subprocess.run(['make', '-C', str(ROOT), 'all', 'CFLAGS=-Werror', 'BUILD=' + str(build)],
                   check=True, env=env)
    tests = Acceptance(build)
    tests.transfer()
    tests.remove_and_names()
    tests.external_and_info()
    tests.attributes()
    tests.tree_and_find()
    tests.live_progress()
    tests.spaced_names()
    tests.smoke_templates()
    print(f'T2 checks passed: {tests.count} scripts, {tests.snapshots} validated snapshots and filesystem assertions')


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError, subprocess.CalledProcessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
