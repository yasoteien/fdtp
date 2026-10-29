#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Stage 1b acceptance: archive browser, u/U/p and the launcher; revision 1 D:
the p box (format, level, LHa names, same-name rule, progress) and the
archives it makes, unpacked with tar, unzip and lha (Lhasa).

Run from the repository root: python3 -B tests/check_archive.py --build build
Fixtures, snapshots, the extraction cache ($FDTP_CACHE_DIR) and $HOME all
stay under BUILD/archive-fixture. Archives are made with tar/zip and Python.
Results are judged from the snapshots and the file system.
"""
import argparse
import io
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tarfile
import time
import zipfile

from check import check_snapshot, notice, popup_rows, require, screen_path, text
from lha_read import lha_contents, lha_read

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / 'tests'


def rows(snap, first=6, last=22):
    return [text(snap, r).rstrip() for r in range(first, last + 1)]


def names(snap):
    """Names in the list (two columns: name at +2, 12 cells), '..' included."""
    out = []
    for r in range(6, 23):
        for base in (0, 40):
            cell = text(snap, r, base + 2, base + 14).strip()
            if cell:
                out.append(cell)
    return out


def path_line(snap):
    return text(snap, 3, 7, 51).strip()


# ---- archives ----

def crc16(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c


def dos_time(t):
    lt = time.localtime(t)
    return ((lt.tm_year - 1980) << 25) | (lt.tm_mon << 21) | (lt.tm_mday << 16) | (lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec // 2)


def lzh_level0(entries):
    out = b''
    for name, data in entries:
        n = name.encode()
        body = b'-lh0-' + struct.pack('<IIIBBB', len(data), len(data), dos_time(1700000000), 0x20, 0, len(n)) + n + struct.pack('<H', crc16(data))
        out += bytes([len(body), sum(body) & 0xff]) + body + data
    return out + b'\0'


def lzh_level2(entries):
    out = b''
    for directory, name, data in entries:
        exts = [b'\x01' + name.encode()]
        if directory:
            exts.append(b'\x02' + directory.encode().replace(b'/', b'\xff') + b'\xff')
        exts.append(b'\x50' + struct.pack('<H', 0o100644))
        common = b'\x00\x00\x00'  # header CRC, filled below
        blocks = [common] + exts
        ext = b''.join(struct.pack('<H', len(b) + 2) + b for b in blocks) + b'\0\0'
        base_len = 24
        total = base_len + len(ext)
        head = struct.pack('<H', total) + b'-lh0-' + struct.pack('<IIIBB', len(data), len(data), 1700000000, 0x20, 2) + struct.pack('<H', crc16(data)) + b'U'
        header = bytearray(head + ext)
        # The common extension (type 0) carries the CRC of the whole header.
        crc = crc16(bytes(header))
        at = base_len + 2 + 1
        header[at:at + 2] = struct.pack('<H', crc)
        out += bytes(header) + data
    return out + b'\0'


def tar_add(archive, name, data=b'', kind=tarfile.REGTYPE, link=''):
    info = tarfile.TarInfo(name)
    info.type = kind
    info.size = len(data) if kind == tarfile.REGTYPE else 0
    info.linkname = link
    info.mtime = 1700000000
    info.mode = 0o755 if kind == tarfile.DIRTYPE else 0o644
    archive.addfile(info, io.BytesIO(data) if kind == tarfile.REGTYPE else None)


def make_tree(base):
    """The sample tree: Japanese name, a deeper level, an empty directory, a link."""
    (base / 'sub' / '深い').mkdir(parents=True)
    (base / 'empty').mkdir()
    (base / '日本語.txt').write_text('hello\n')
    (base / 'sub' / '深い' / 'a.txt').write_text('deep a\n')
    (base / 'sub' / 'link').symlink_to('../日本語.txt')


class Acceptance:
    def __init__(self, build):
        self.build = build
        self.base = build / 'archive-fixture'
        if self.base.exists():
            shutil.rmtree(self.base)
        self.base.mkdir(parents=True)
        self.executable = build / ('fdtp-preview' if sys.platform == 'darwin' else 'fdtp')
        self.scripts = 0
        self.snapshots = 0
        self.sample = self.base / 'sample'
        self.sample.mkdir()
        make_tree(self.sample)

    def case(self, name):
        home = self.base / name
        for child in ('w', 'home', 'cache'):
            (home / child).mkdir(parents=True)
        return home, home / 'w'

    def env(self, home):
        return dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(self.build / 'tmp'), TZ='UTC', HOME=str(home / 'home'),
                    FDTP_CACHE_DIR=str(home / 'cache'), SHELL='/bin/sh')

    def run(self, home, script_name, prefix='screen', extra_env=None):
        source = (TESTS / script_name).read_text()
        labels = re.findall(r'\bSNAP\s+#\s*([a-z0-9_]+)', source)
        require(len(labels) == len(set(labels)), f'{script_name}: duplicate labels')
        require(len(labels) == len(re.findall(r'\bSNAP\b', source)), f'{script_name}: every SNAP needs a label')
        escaped = str(home).replace('\\', '\\\\').replace('"', '\\"')
        script = home / script_name
        script.write_text(source.replace('@CASE@', escaped))
        out = home / prefix
        subprocess.run([str(self.executable), '--headless', '--script', str(script), '--out', str(out),
                        '--time', '1700000000', str(home / 'w')], check=True, cwd=ROOT.parent,
                       env=dict(self.env(home), **(extra_env or {})), stdout=subprocess.DEVNULL)
        files = sorted(home.glob(f'{prefix}-*.cells'))
        require(len(files) == len(labels), f'{script_name}: expected {len(labels)} snapshots, got {len(files)}')
        self.scripts += 1
        self.snapshots += len(files)
        print(f'  {script_name}: {len(files)} snapshots')
        # The cache is emptied when the browsers close (the program has ended).
        require(not any((home / 'cache').iterdir()), f'{script_name}: extraction cache left behind')
        return {label: check_snapshot(files[i]) for i, label in enumerate(labels)}

    def archives(self, w):
        subprocess.run(['tar', '-czf', str(w / 't.tgz'), '-C', str(self.sample), '.'], check=True)
        subprocess.run(['tar', '-cf', str(w / 't.tar'), '-C', str(self.sample), '日本語.txt', 'sub', 'empty'], check=True)
        subprocess.run(['zip', '-q', '-r', '-y', str(w / 'z.zip'), '.'], check=True, cwd=self.sample)

    def tree_matches(self, dst, what):
        require((dst / '日本語.txt').read_text() == 'hello\n', f'{what}: Japanese name and data')
        require((dst / 'sub' / '深い' / 'a.txt').read_text() == 'deep a\n', f'{what}: deeper level')
        require((dst / 'empty').is_dir() and not any((dst / 'empty').iterdir()), f'{what}: empty directory')
        require(os.readlink(dst / 'sub' / 'link') == '../日本語.txt', f'{what}: symbolic link')

    # ---- scenarios ----

    def browse(self):
        home, w = self.case('browse')
        self.archives(w)
        (w / 'note.txt').write_text('plain note\n')
        with zipfile.ZipFile(w / 'nest.zip', 'w') as z:
            z.write(w / 't.tgz', 'inner.tgz')
        # A Shift_JIS name without the UTF-8 flag (as Windows writes it):
        # written under an ASCII name of the same length, then patched.
        sjis = '日本.txt'.encode('cp932')
        with zipfile.ZipFile(w / 'sjis.zip', 'w') as z:
            z.writestr('X' * (len(sjis) - 4) + '.txt', 'sjis\n')
        data = (w / 'sjis.zip').read_bytes().replace(('X' * (len(sjis) - 4) + '.txt').encode(), sjis)
        (w / 'sjis.zip').write_bytes(data)
        s = self.run(home, 'archive_browse.keys')
        root = s['tgz_root']
        require(s['tgz_root']['depth'] == 2, 'RET on an archive pushes the browser')
        require(names(root)[:4] == ['..', 'empty', 'sub', '日本語  .txt'], f'tgz root list: {names(root)}')
        require(path_line(root).endswith('t.tgz:/'), f'Path shows the archive: {path_line(root)}')
        require('<DIR>' in rows(root)[1], 'empty directory shown as a directory')
        require(names(s['tgz_sub'])[:3] == ['..', '深い', 'link'] and path_line(s['tgz_sub']).endswith('t.tgz:/sub'), 'into sub')
        require(names(s['tgz_deep'])[:2] == ['..', 'a       .txt'] and path_line(s['tgz_deep']).endswith('/sub/深い'), 'into the deeper level')
        require(text(s['tgz_up'], 4, 7, 24).strip() == '深い', 'BS goes up with the cursor on where it came from')
        require(text(s['tgz_top'], 4, 7, 24).strip() == 'sub' and s['tgz_top']['depth'] == 2, 'BS to the archive top')
        opened = s['tgz_open']
        # The text view's body starts on row 1, inside the screen's frame (revision 1 item 29).
        require(opened['depth'] == 3 and '日本語.txt' in text(opened, 0) and 'hello' in text(opened, 1),
                f'member opened in the text view: {text(opened, 0)!r} {text(opened, 1)!r}')
        require(s['tgz_back']['depth'] == 2, 'q returns to the browser')
        require(s['tgz_left']['depth'] == 1 and 't.tgz' in text(s['tgz_left'], 4), 'BS at the top leaves the browser')
        z = s['zip_open']
        require(z['depth'] == 3 and 'a.txt' in text(z, 0) and 'deep a' in text(z, 1), 'zip member below two levels')
        require(text(s['zip_link_cursor'], 4, 7, 24).strip() == 'link', 'zip: cursor on the link')
        require(s['zip_link_open']['depth'] == 3 and 'hello' in text(s['zip_link_open'], 1), 'a link opens its target in the archive')
        require(s['zip_left']['depth'] == 1, 'Esc leaves the browser')
        m = s['tar_marked']
        require(int(text(m, 3, 52, 58).strip() or 0) == 1 and text(m, 9, 1, 2) == '*', 'mark in the browser')
        require(s['v_archive']['depth'] == 2 and path_line(s['v_archive']).endswith('t.tgz:/'), 'v on an archive browses it')
        require(s['v_text']['depth'] == 2 and 'plain note' in text(s['v_text'], 1), 'v on text: the text view')
        n = s['nested']
        require(n['depth'] == 3 and path_line(n).endswith('inner.tgz:/'), f'archive inside an archive: {path_line(n)!r}')
        require(names(n)[:4] == ['..', 'empty', 'sub', '日本語  .txt'], f'nested list: {names(n)}')
        require(s['nested_open']['depth'] == 4 and 'hello' in text(s['nested_open'], 1), 'member of the nested archive')
        require(s['nested_left']['depth'] == 1, 'both browsers closed')
        require(text(s['sjis_name'], 4, 7, 24).strip() == '日本.txt', f'Shift_JIS name shown: {text(s["sjis_name"], 4, 7, 24)!r}')
        require(s['done']['depth'] == 1, 'back in the filer')

    def unpack(self):
        home, w = self.case('unpack')
        self.archives(w)
        (w / 'note.txt').write_text('x\n')
        for i in range(1, 6):
            (home / f'dst{i}').mkdir()
        s = self.run(home, 'archive_unpack.keys')
        require(notice(s['u_prompt']).startswith('解凍先') and notice(s['u_prompt']).endswith('/unpack/w'), f'u asks, current directory first: {notice(s["u_prompt"])!r}')
        require(notice(s['u_tgz']).startswith('解凍しました'), f'u result: {notice(s["u_tgz"])!r}')
        self.tree_matches(home / 'dst1', 'u tgz')
        self.tree_matches(home / 'dst2', 'u zip')
        require('解凍先のツリー' in text(s['tree_start'], 0) and text(s['tree_start'], 21).strip() == screen_path(w.resolve(), home=home / 'home'), 'U: tree at the current directory')
        require(text(s['tree_dst3'], 21).strip() == screen_path((home / 'dst3').resolve(), home=home / 'home'), f'tree cursor: {text(s["tree_dst3"], 21)!r}')
        self.tree_matches(home / 'dst3', 'U tgz')
        d4 = home / 'dst4'
        require((d4 / 'sub' / '深い' / 'a.txt').exists() and (d4 / 'sub' / 'link').is_symlink(), 'marked sub extracted')
        require(not (d4 / '日本語.txt').exists() and not (d4 / 'empty').exists(), 'only the marked entry')
        require(notice(s['u_marked']).startswith('解凍しました'), f'marked result: {notice(s["u_marked"])!r}')
        self.tree_matches(home / 'dst5', 'U in the browser, nothing marked')
        require(notice(s['u_missing']) == '行先のディレクトリがありません' and not (home / 'nowhere').exists(), 'missing destination')
        require(notice(s['u_not_archive']) == 'アーカイブではありません', 'u on a plain file')
        require(notice(s['u_cancel']) == '操作を中止しました' and s['u_cancel']['depth'] == 1, 'Esc cancels')

    def listing(self, archive):
        if archive.suffix == '.zip':
            # Without the UTF-8 flag (Info-ZIP built without Unicode support,
            # as on the Mac) Python reads the stored bytes as CP437.
            infos = zipfile.ZipFile(archive).infolist()
            return sorted((i.filename if i.flag_bits & 0x800 else i.filename.encode('cp437').decode('utf-8')).rstrip('/') for i in infos)
        with tarfile.open(archive) as t:
            return sorted(n.removeprefix('./').rstrip('/') for n in t.getnames())

    # ---- the p box (revision 1 D) ----

    def extract(self, archive, into):
        """Unpack with the outside commands: tar, unzip, lha (Lhasa)."""
        if into.exists():
            shutil.rmtree(into)
        into.mkdir(parents=True)
        name = archive.name
        if name.endswith('.zip'):
            subprocess.run(['unzip', '-q', str(archive), '-d', str(into)], check=True)
        elif name.endswith('.lzh'):
            subprocess.run([self.lha_command(), 'xq', str(archive.resolve())], check=True, cwd=into)
        else:
            subprocess.run(['tar', '-xf', str(archive), '-C', str(into)], check=True)
        return into

    def lha_command(self):
        lha = shutil.which('lhasa') or shutil.which('lha')
        require(lha, 'lha (Lhasa) is needed to check the LHa archives')
        return lha

    def same_tree(self, src, dst, what, times=True):
        """Every file, directory and link of src is in dst: bytes, link targets, file times."""
        for path in sorted(src.rglob('*')):
            rel = path.relative_to(src)
            other = dst / rel
            require(other.exists() or other.is_symlink(), f'{what}: {rel} missing')
            if path.is_symlink():
                require(other.is_symlink() and os.readlink(other) == os.readlink(path), f'{what}: link {rel}')
            elif path.is_dir():
                require(other.is_dir() and not other.is_symlink(), f'{what}: directory {rel}')
            else:
                require(other.read_bytes() == path.read_bytes(), f'{what}: bytes of {rel}')
                if times:
                    require(int(other.stat().st_mtime) == int(path.stat().st_mtime), f'{what}: time of {rel}')
        extra = sorted(str(p.relative_to(dst)) for p in dst.rglob('*') if not (src / p.relative_to(dst)).exists() and not (src / p.relative_to(dst)).is_symlink())
        require(not extra, f'{what}: unexpected {extra}')

    def pack(self):
        home, w = self.case('pack')
        bin_dir = home / 'home' / '.fdtp' / 'bin'
        bin_dir.mkdir(parents=True)
        (bin_dir / 'lhasa').symlink_to(self.lha_command())
        box = w / 'box'
        make_tree(box)
        (box / '空.txt').write_bytes(b'')
        (w / 'a.txt').write_text('a\n')
        (w / 'b.txt').write_text('b\n')
        (w / 'rep.txt').write_bytes(b''.join(b'line %05d of a repeated text, the same words again and again\n' % (i % 997) for i in range(40000)))
        for p in [box, *box.rglob('*')]:
            os.utime(p, (1600000000, 1600000000), follow_symlinks=False)
        s = self.run(home, 'archive_pack.keys')
        BOXROW = 4

        def field(snap):
            return text(snap, BOXROW + 3, 7, 74).strip()

        def chosen_format(snap):
            got = [text(snap, BOXROW + 6 + i, 7, 16).strip() for i in range(6) if snap['rows'][BOXROW + 6 + i][6][2] != 0]
            return got[0] if len(got) == 1 else got

        def focus(snap):
            labels = {'書庫名': (BOXROW + 2, 6), '形式': (BOXROW + 5, 6), '圧縮率': (BOXROW + 5, 22), '名前': (BOXROW + 8, 22), '同名': (BOXROW + 10, 22)}
            return [k for k, (r, c) in labels.items() if snap['rows'][r][c][2] == 1]

        def level(snap):
            m = re.search(r'高\s+(\d)', text(snap, BOXROW + 6))
            return int(m.group(1)) if m else None

        def newer(snap):
            return snap['rows'][BOXROW + 12][24][2] != 0

        def utf8(snap):
            return snap['rows'][BOXROW + 8][35][2] != 0

        b = s['p_box']
        require(b['depth'] == 2 and b['boxes'][0][:4] == [4, 4, 72, 17] and b['boxes'][0][5] == 3, f'p box with frame and shadow: {b["boxes"]}')
        require('書庫の作成' in text(b, BOXROW + 1) and '↓ 履歴' in text(b, BOXROW + 1), 'title and the history hint')
        require(field(b) == 'box.tar.gz' and chosen_format(b) == 'TAR.GZ' and focus(b) == ['書庫名'], f'first state: {field(b)!r} {chosen_format(b)} {focus(b)}')
        require(level(b) == 6 and text(b, BOXROW + 6).count('■') == 6 and text(b, BOXROW + 6).count('□') == 3, f'level 6 of 9: {text(b, BOXROW + 6)!r}')
        require([text(b, BOXROW + 6 + i, 7, 16).strip() for i in range(6)] == ['LHA', 'ZIP', 'TAR', 'TAR.GZ', 'TAR.BZ2', 'TAR.XZ'], 'formats listed')
        require(not utf8(b) and '（LHA の時）' in text(b, BOXROW + 8) and not newer(b), 'SJIS (only for LHA) and overwrite by default')
        require(focus(s['fmt_focus']) == ['形式'], 'TAB to the formats')
        for label, name, fmt in (('fmt_tar', 'box.tar', 'TAR'), ('fmt_zip', 'box.zip', 'ZIP'), ('fmt_lha', 'box.lzh', 'LHA'), ('fmt_top', 'box.lzh', 'LHA'),
                                 ('fmt_txz', 'box.tar.xz', 'TAR.XZ'), ('fmt_bottom', 'box.tar.xz', 'TAR.XZ')):
            require(field(s[label]) == name and chosen_format(s[label]) == fmt, f'{label}: {field(s[label])!r} {chosen_format(s[label])}')
        require('（LHA の時）' not in text(s['fmt_lha'], BOXROW + 8), 'the name code is open for LHA')
        require(focus(s['level_4']) == ['圧縮率'] and level(s['level_4']) == 4, 'level down with ←')
        require(level(s['level_9']) == 9 and text(s['level_9'], BOXROW + 6).count('■') == 9, 'level stops at 9')
        require(level(s['level_3']) == 3, 'a digit sets the level')
        require(focus(s['same_focus']) == ['同名'], f'TAB skips the name code for TAR.XZ: {focus(s["same_focus"])}')
        require(newer(s['same_newer']) and not newer(s['same_focus']), 'same-name rule chosen')
        require(focus(s['name_focus']) == ['書庫名'], 'TAB comes round to the name')
        require(focus(s['back_tab']) == ['同名'], f'Shift+TAB goes back: {focus(s["back_tab"])}')
        c = s['p_cancel']
        require(c['depth'] == 1 and notice(c) == '操作を中止しました', 'Esc cancels')
        r = s['p_remembered']
        require(field(r) == 'box.tar.xz' and level(r) == 3 and newer(r), f'choices kept for the next p: {field(r)!r} {level(r)} {newer(r)}')
        t = s['fmt_tar_level']
        require(chosen_format(t) == 'TAR' and level(t) is None and 'TAR は圧縮しません' in text(t, BOXROW + 6), 'no level for TAR')
        require(focus(s['tar_tab']) == ['同名'], 'TAB skips the level for TAR')
        require(field(s['name_typed']) == 'abc.zip' and chosen_format(s['name_typed']) == 'ZIP', 'a typed extension chooses the format')
        require(s['p_cancel2']['depth'] == 1, 'second cancel')
        created = ['p_zip', 'p_tar', 'p_bz2', 'p_xz', 'p_tgz', 'p_lha', 'p_lha_utf8', 'p_g1', 'p_g9', 'p_z1', 'p_z9', 'p_l1', 'p_l9', 'p_noext', 'p_marked']
        for label in created:
            require(s[label]['depth'] == 1 and notice(s[label]).startswith('圧縮しました'), f'{label}: {notice(s[label])!r}')
        want = ['box', 'box/empty', 'box/sub', 'box/sub/link', 'box/sub/深い', 'box/sub/深い/a.txt', 'box/日本語.txt', 'box/空.txt']
        for name in ('out.zip', 'out.tar', 'out.tar.bz2', 'out.tar.xz', 'out.tgz'):
            got = self.listing(w / name)
            require(got == want, f'{name}: {got}')
            self.same_tree(box, self.extract(w / name, home / 'x' / name) / 'box', name)
        with tarfile.open(w / 'out.tgz') as t:
            require(t.getmember('box/sub/link').issym(), 'tar keeps the link')
        with zipfile.ZipFile(w / 'out.zip') as z:
            byname = {(i.filename if i.flag_bits & 0x800 else i.filename.encode('cp437').decode('utf-8')): i for i in z.infolist()}
            require((byname['box/sub/link'].external_attr >> 16) & 0o170000 == 0o120000, 'zip keeps the link (-y)')
        require(notice(s['p_lha']).startswith('圧縮しました: box.lzh（8 個'), f'LHa notice: {notice(s["p_lha"])!r}')
        require(field(s['lha_chosen']) == 'box.lzh' and chosen_format(s['lha_chosen']) == 'LHA', 'LHA chosen in the list')
        sjis = lha_read(w / 'box.lzh')
        require(sorted(m['path'] for m in sjis) == want, f'Shift_JIS names: {sorted(m["path"] for m in sjis)}')
        require(all(m['level'] == 2 and m['os'] == 'U' and m['header_crc_ok'] and m['encoding'] in ('ascii', 'cp932') for m in sjis)
                and sum(m['encoding'] == 'cp932' for m in sjis) == 4, 'level 2, header CRC, Shift_JIS names')
        require({m['path']: m['method'] for m in sjis}['box/sub'] == '-lhd-' and {m['path']: m['link'] for m in sjis}['box/sub/link'] == '../日本語.txt', 'directories and the link')
        require(lha_contents(self.lha_command(), w / 'box.lzh', sjis) == {m['path']: (box / m['path'][4:]).read_bytes() for m in sjis if m['method'] != '-lhd-'}, 'Shift_JIS archive data (lha p)')
        require(utf8(s['code_utf8']) and focus(s['code_utf8']) == ['名前'], 'UTF-8 chosen')
        u = lha_read(w / 'utf.lzh')
        require(sorted(m['path'] for m in u) == want and sum(m['encoding'] == 'utf-8' for m in u) == 4 and all(m['encoding'] != 'cp932' for m in u), 'UTF-8 names')
        self.same_tree(box, self.extract(w / 'utf.lzh', home / 'x' / 'utf') / 'box', 'utf.lzh')
        sizes = {n: (w / n).stat().st_size for n in ('g1.tar.gz', 'g9.tar.gz', 'z1.zip', 'z9.zip', 'l1.lzh', 'l9.lzh')}
        require(sizes['g9.tar.gz'] < sizes['g1.tar.gz'] and sizes['z9.zip'] < sizes['z1.zip'] and sizes['l9.lzh'] < sizes['l1.lzh'], f'level 9 is smaller than 1: {sizes}')
        require((w / 'g1.tar.gz').read_bytes()[8] == 4 and (w / 'g9.tar.gz').read_bytes()[8] == 2, 'gzip -1 and -9 (XFL byte)')
        for n in ('g1.tar.gz', 'z9.zip', 'l1.lzh', 'l9.lzh'):
            self.same_tree(w / 'rep.txt', self.extract(w / n, home / 'x' / n) / 'rep.txt', n)
        self.levels = sizes
        self.rep_size = (w / 'rep.txt').stat().st_size
        require((w / 'plain.lzh').exists() and notice(s['p_noext']).startswith('圧縮しました: plain.lzh'), 'the format extension is added')
        require(s['p_7z']['depth'] == 2 and s['p_7z']['widget'] == 6 and 'この環境では使えません' in notice(s['p_7z']), f'7z refused: {notice(s["p_7z"])!r}')
        require(s['p_7z_closed']['depth'] == 2 and s['p_7z_closed']['widget'] == 0 and field(s['p_7z_closed']) == 'out.7z', 'the box stays after the error')
        require('名前が空です' in notice(s['p_empty']), f'empty name: {notice(s["p_empty"])!r}')
        require('行先のディレクトリがありません' in notice(s['p_no_dir']), f'missing directory: {notice(s["p_no_dir"])!r}')
        require(s['p_refused_left']['depth'] == 1, 'left the box')
        for n in ('out.7z', 'out.7z.lzh', 'nodir', '.lzh'):
            require(not (w / n).exists(), f'nothing made: {n}')
        e = s['p_exists']
        require('out.tgz はあります。書庫に追加しますか？' in notice(e) and e['widget'] == 2, f'existing archive: {notice(e)!r}')
        require(s['p_exists_no']['depth'] == 2 and s['p_exists_no']['widget'] == 0, 'n goes back to the box')
        require(field(s['p_marked_prompt']) == 'w.tar.gz', f'marked: the directory name: {field(s["p_marked_prompt"])!r}')
        require(self.listing(w / 'marked.tar') == ['a.txt', 'b.txt'], f'marked files: {self.listing(w / "marked.tar")}')
        m = s['browse_made']
        require(m['depth'] == 2 and path_line(m).endswith('utf.lzh:/') and names(m)[:2] == ['..', 'box'], f'browse a made archive: {names(m)}')
        b2 = s['browse_sub']
        require(path_line(b2).endswith('utf.lzh:/box/sub') and names(b2)[:3] == ['..', '深い', 'link'], f'its sub directory (link by name): {names(b2)}')
        require(s['browse_left']['depth'] == 1, 'left the browser')
        h = s['p_history']
        listed = [y for y, t in popup_rows(h) if t.endswith('marked.tar')]
        require(len(h['boxes']) == 2 and listed, f'history list under the name: {h["boxes"]} {popup_rows(h)}')
        require(field(s['p_history_pick']) == 'out.tgz' and len(s['p_history_pick']['boxes']) == 1, f'picked from the history: {field(s["p_history_pick"])!r}')
        require(s['p_list_closed']['depth'] == 2 and len(s['p_list_closed']['boxes']) == 1, 'Esc closes only the history list')
        require(s['done']['depth'] == 1, 'filer after p')
        require(not list(w.glob('*.tmp*')), 'no temporary files left')

    def pack_add(self):
        home, w = self.case('pack_add')
        box = w / 'box'
        make_tree(box)
        for p in [box, *box.rglob('*')]:
            os.utime(p, (1600000000, 1600000000), follow_symlinks=False)
        self.run(home, 'archive_pack_add1.keys', prefix='first')
        # 日本語.txt changes but is dated earlier; a.txt changes later; new.txt is new.
        (box / '日本語.txt').write_text('changed but old\n')
        os.utime(box / '日本語.txt', (1500000000, 1500000000))
        (box / 'sub' / '深い' / 'a.txt').write_text('newer a\n')
        os.utime(box / 'sub' / '深い' / 'a.txt', (1700000000, 1700000000))
        (box / 'new.txt').write_text('new file\n')
        os.utime(box / 'new.txt', (1700000000, 1700000000))
        s = self.run(home, 'archive_pack_add2.keys', prefix='second')
        require('all.lzh はあります。書庫に追加しますか？' in notice(s['all_lzh_ask']), f'asks before adding: {notice(s["all_lzh_ask"])!r}')
        require(s['newer_chosen']['rows'][4 + 12][24][2] != 0, '日付が新しい時だけ chosen')
        for label in ('all_lzh', 'all_zip', 'all_tgz', 'new_lzh', 'new_zip', 'new_tgz'):
            require(s[label]['depth'] == 1 and notice(s[label]).startswith('書庫に追加しました'), f'{label}: {notice(s[label])!r}')
        require('古いので入れなかった物' in notice(s['new_lzh']), f'newer only reports the older: {notice(s["new_lzh"])!r}')
        for ext in ('lzh', 'zip', 'tgz'):
            a = self.extract(w / f'all.{ext}', home / 'x' / f'all.{ext}') / 'box'
            require((a / '日本語.txt').read_text() == 'changed but old\n', f'all.{ext}: replaced unconditionally')
            require((a / 'sub' / '深い' / 'a.txt').read_text() == 'newer a\n' and (a / 'new.txt').read_text() == 'new file\n', f'all.{ext}: newer and new')
            require((a / 'empty').is_dir() and os.readlink(a / 'sub' / 'link') == '../日本語.txt', f'all.{ext}: the rest kept')
            n = self.extract(w / f'new.{ext}', home / 'x' / f'new.{ext}') / 'box'
            require((n / '日本語.txt').read_text() == 'hello\n', f'new.{ext}: the older file did not replace the member')
            require((n / 'sub' / '深い' / 'a.txt').read_text() == 'newer a\n' and (n / 'new.txt').read_text() == 'new file\n', f'new.{ext}: newer and new')
        for ext in ('lzh',):
            names = [m['path'] for m in lha_read(w / f'all.{ext}')]
            require(len(names) == len(set(names)), f'all.{ext}: no member twice: {names}')
        require(not any((home / 'cache').iterdir()), 'tar staging removed')
        require(not list(w.glob('*.tmp*')), 'no temporary files left')

    def pack_progress(self):
        home, w = self.case('pack_progress')
        big = w / 'big'
        big.mkdir()
        for i in range(3):
            (big / f'part{i}.bin').write_bytes(os.urandom(150000) + b'x' * 150000)
        (w / 'face😀.txt').write_text('smile\n')
        s = self.run(home, 'archive_pack_progress.keys', extra_env={'FDTP_PACK_SLICE': '100000'})
        r = s['sjis_refused']
        require(r['depth'] == 2 and r['widget'] == 6 and 'Shift_JIS にできない名前' in notice(r), f'Shift_JIS refusal: {notice(r)!r}')
        require(notice(s['utf8_made']).startswith('圧縮しました: face.lzh（1 個'), f'UTF-8 takes it: {notice(s["utf8_made"])!r}')
        require([m['path'] for m in lha_read(w / 'face.lzh')] == ['face😀.txt'], 'the UTF-8 name')
        f = s['run_first']
        require(f['depth'] == 2 and text(f, 18).strip().startswith('圧縮中 2 / 4 個') and 'big/part0.bin' in text(f, 18), f'progress: {text(f, 18)!r}')
        def percent(snap):
            m = re.search(r'(\d+)%', text(snap, 19))
            return int(m.group(1)) if m else -1
        require(0 < percent(f) < percent(s['run_second']) < 100 and 'Esc で中止' in text(f, 19), f'percent grows: {percent(f)} {percent(s["run_second"])}')
        require(text(f, 19).count('■') >= 1, 'progress bar')
        require(s['run_stopped']['depth'] == 1 and notice(s['run_stopped']) == '圧縮を中止しました', 'Esc stops')
        # The keys after the end went to the filer (and cleared the notice).
        require(s['run_done']['depth'] == 1 and (w / 'big.lzh').exists(), 'finished within the ticks')
        self.same_tree(big, self.extract(w / 'big.lzh', home / 'x') / 'big', 'big.lzh')
        require(not list(w.glob('*.tmp*')), 'the stopped job left no temporary file')

    def bad(self):
        home, w = self.case('bad')
        (home / 'dst').mkdir()
        with tarfile.open(w / 'bad.tar', 'w', format=tarfile.GNU_FORMAT) as t:
            tar_add(t, 'ok.txt', b'fine\n')
            tar_add(t, '../evil.txt', b'evil\n')
            tar_add(t, '/abs/evil2.txt', b'evil\n')
        with tarfile.open(w / 'esc.tar', 'w', format=tarfile.GNU_FORMAT) as t:
            tar_add(t, 'esc', kind=tarfile.SYMTYPE, link='..')
            tar_add(t, 'esc/pwn.txt', b'pwn\n')
        with zipfile.ZipFile(w / 'bad.zip', 'w') as z:
            z.writestr('fine.txt', 'fine\n')
            z.writestr('../zevil.txt', 'evil\n')
        good = io.BytesIO()
        with zipfile.ZipFile(good, 'w') as z:
            z.writestr('a.txt', 'a' * 300)
            z.writestr('b.txt', 'b' * 300)
        data = good.getvalue()
        (w / 'broken.zip').write_bytes(data[:len(data) // 2])
        corrupt = bytearray(data)
        eocd = corrupt.rfind(b'PK\x05\x06')
        corrupt[eocd + 16:eocd + 20] = struct.pack('<I', 7)  # central directory offset into file data
        (w / 'cd.zip').write_bytes(bytes(corrupt))
        (w / 'empty.zip').write_bytes(b'')
        (w / 'broken.tgz').write_bytes(b'\x1f\x8b\x08\x00' + os.urandom(200))
        (w / 'x.7z').write_bytes(b"7z\xbc\xaf'\x1c")
        (w / 'l.lzh').write_bytes(lzh_level0([('a.txt', b'a\n')]))
        # no_lhasa: PATH without lhasa, nor an lha that is Lhasa (Homebrew's name).
        def lhasa_in(d):
            if (Path(d) / 'lhasa').exists(): return True
            lha = Path(d) / 'lha'
            if not lha.exists(): return False
            r = subprocess.run([str(lha)], capture_output=True)
            return b'Lhasa' in r.stdout + r.stderr
        path = os.pathsep.join(d for d in os.environ.get('PATH', '').split(os.pathsep) if d and not lhasa_in(d))
        s = self.run(home, 'archive_bad.keys', extra_env={'PATH': path})
        dst = home / 'dst'
        require(s['bad_browse']['depth'] == 2 and '外に出る名前が 2 個' in notice(s['bad_browse']), f'bad listing notice: {notice(s["bad_browse"])!r}')
        require(names(s['bad_root']) == ['..', '../evil .txt', '/abs/evi.txt', 'ok      .txt'], f'bad root (unsafe names at the top, as stored): {names(s["bad_root"])}')
        require(notice(s['bad_open_evil']).startswith('危険な名前があるので取り出しません') and s['bad_open_evil']['depth'] == 2, 'RET on ../ refused')
        require(s['bad_open_ok']['depth'] == 3 and 'fine' in text(s['bad_open_ok'], 1), 'a safe member of a bad archive opens')
        for label in ('bad_u_browser', 'bad_u', 'esc_u', 'badzip_u'):
            require(notice(s[label]).startswith('危険な名前があるので取り出しません'), f'{label}: {notice(s[label])!r}')
        require(not any(dst.iterdir()), f'nothing extracted into dst: {list(dst.iterdir())}')
        for p in (home / 'evil.txt', home / 'zevil.txt', home / 'pwn.txt', self.base / 'evil.txt', Path('/abs')):
            require(not p.exists(), f'written outside: {p}')
        for label in ('broken_tgz', 'broken_zip', 'broken_cd', 'empty_zip'):
            require(s[label]['depth'] == 1 and notice(s[label]).startswith('アーカイブを読めません'), f'{label}: {notice(s[label])!r}')
        require(notice(s['broken_u']).startswith('アーカイブを読めません'), f'broken u: {notice(s["broken_u"])!r}')
        require(notice(s['sevenz']) == 'この環境では使えません' and s['sevenz']['depth'] == 1, '7z browse')
        require(notice(s['sevenz_u']) == 'この環境では使えません', '7z unpack')
        require(notice(s['no_lhasa']) == 'lhasa がありません', f'no lhasa: {notice(s["no_lhasa"])!r}')

    def lha(self):
        lhasa = shutil.which('lhasa') or shutil.which('lha')
        if not lhasa or b'Lhasa' not in subprocess.run([lhasa], capture_output=True).stdout + subprocess.run([lhasa], capture_output=True).stderr:
            print('  archive_lha.keys: skipped (no lhasa on this machine)')
            return 0
        home, w = self.case('lha')
        (home / 'home' / '.fdtp' / 'bin').mkdir(parents=True)
        (home / 'home' / '.fdtp' / 'bin' / 'lhasa').symlink_to(lhasa)
        (home / 'dst').mkdir()
        (home / 'dst2').mkdir()
        (w / 'l.lzh').write_bytes(lzh_level0([('a.txt', b'lzh a\n'), ('sub\\b.txt', b'lzh b\n'), ('日本語.txt', b'lzh jp\n')]))
        (w / 'l2.lzh').write_bytes(lzh_level2([('', 'top.txt', b'top\n'), ('dir/深い', 'c.txt', b'lzh c\n')]))
        (w / 'garbage.lzh').write_bytes(b'not an lzh archive at all\n')
        s = self.run(home, 'archive_lha.keys')
        require(names(s['lzh_root'])[:4] == ['..', 'sub', 'a       .txt', '日本語  .txt'], f'lzh root: {names(s["lzh_root"])}')
        require(s['lzh_open']['depth'] == 3 and 'lzh a' in text(s['lzh_open'], 1), 'lzh member opened')
        require(names(s['lzh_sub'])[:2] == ['..', 'b       .txt'], f'lzh sub: {names(s["lzh_sub"])}')
        dst = home / 'dst'
        require((dst / 'a.txt').read_text() == 'lzh a\n' and (dst / 'sub' / 'b.txt').read_text() == 'lzh b\n'
                and (dst / '日本語.txt').read_text() == 'lzh jp\n', f'lzh u: {sorted(str(p) for p in dst.rglob("*"))}')
        d2 = home / 'dst2'
        require((d2 / 'top.txt').read_text() == 'top\n' and (d2 / 'dir' / '深い' / 'c.txt').read_text() == 'lzh c\n',
                f'level 2 u: {sorted(str(p) for p in d2.rglob("*"))}')
        require(s['lzh_broken']['depth'] == 1 and notice(s['lzh_broken']).startswith('アーカイブを読めません'), f'lzh broken: {notice(s["lzh_broken"])!r}')
        return 1


LAUNCHER_HARNESS = r'''
#include "launcher.h"
#include "app.h"
#include <stdio.h>
#include <string.h>
static const char *last; static char external[1024], cwd[1024];
void app_notice(App *a,const char *m) { (void)a; last=m; }
void app_error(App *a,const char *m) { (void)a; last=m; }
int app_push(App *a,Screen *s) { (void)a; (void)s; return 0; }
/* Revision 1 B: the binary-file question (launcher.c) also uses these. */
void app_pop(App *a) { (void)a; }
Screen *app_top(App *a) { (void)a; return NULL; }
void widgets_choice(Widgets *w,const char *l,const char *const *o,int n) { (void)w; (void)l; (void)o; (void)n; }
WidgetResult widgets_key(Widgets *w,const Key *k) { (void)w; (void)k; return WIDGET_PASS; }
int app_request_external_at(App *a,const char *const *argv,unsigned flags,const char *dir)
{ (void)a; (void)flags; snprintf(external,sizeof(external),"%s|%s|%s",argv[0],argv[1],argv[2]); snprintf(cwd,sizeof(cwd),"%s",dir?dir:""); return 0; }
static Screen screen; static const char *opened;
#define VIEW(name) Screen *name(App *a,const char *p) { (void)a; (void)p; opened=#name; return &screen; }
VIEW(textview_create) VIEW(imageview_create) VIEW(pdfview_create) VIEW(sheetview_create)
VIEW(docview_create) VIEW(slideview_create) VIEW(noveledit_create) VIEW(hexview_create)
int archive_browse(App *a,const char *p) { (void)a; (void)p; opened="archive_browse"; return 0; }
static int fails;
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"launcher check failed: %s (line %d)\n",#c,__LINE__); ++fails; } } while(0)
int main(int argc,char **argv)
{
    App *app=NULL; const char *dir=argc>1?argv[1]:".";
    struct { const char *name; LauncherKind kind; } kinds[]={
        {"a.tar",LAUNCHER_ARCHIVE},{"a.TAR.GZ",LAUNCHER_ARCHIVE},{"a.tgz",LAUNCHER_ARCHIVE},{"a.tar.bz2",LAUNCHER_ARCHIVE},
        {"a.tbz",LAUNCHER_ARCHIVE},{"a.tar.xz",LAUNCHER_ARCHIVE},{"a.txz",LAUNCHER_ARCHIVE},{"a.zip",LAUNCHER_ARCHIVE},
        {"a.lzh",LAUNCHER_ARCHIVE},{"a.lha",LAUNCHER_ARCHIVE},{"a.7z",LAUNCHER_ARCHIVE},
        {"p.jpg",LAUNCHER_IMAGE},{"p.JPEG",LAUNCHER_IMAGE},{"p.png",LAUNCHER_IMAGE},{"p.gif",LAUNCHER_IMAGE},{"p.bmp",LAUNCHER_IMAGE},
        {"p.webp",LAUNCHER_IMAGE},{"p.HEIC",LAUNCHER_IMAGE},{"p.heif",LAUNCHER_IMAGE},{"p.mag",LAUNCHER_IMAGE},{"p.mki",LAUNCHER_IMAGE},{"p.pi",LAUNCHER_IMAGE},
        {"d.pdf",LAUNCHER_PDF},{"s.csv",LAUNCHER_SHEET},{"s.tsv",LAUNCHER_SHEET},{"s.xlsx",LAUNCHER_SHEET},
        {"w.docx",LAUNCHER_DOC},{"k.pptx",LAUNCHER_SLIDE},{"t.txt",LAUNCHER_TEXT},{"noext",LAUNCHER_TEXT},
        {".zip",LAUNCHER_TEXT},{"x.gz",LAUNCHER_TEXT},{"dir/a.b/c",LAUNCHER_TEXT},
    };
    for(size_t i=0;i<sizeof(kinds)/sizeof(*kinds);++i) if(launcher_kind(kinds[i].name)!=kinds[i].kind) { fprintf(stderr,"kind %s\n",kinds[i].name); ++fails; }
    size_t base=launcher_count(); CHECK(base>=26);
    CHECK(launcher_register(app,"txt","noveledit")==0 && launcher_kind("n.TXT")==LAUNCHER_NOVEL);
    CHECK(launcher_register(app,"*.md","textview")==0 && launcher_kind("r.md")==LAUNCHER_TEXT);
    CHECK(launcher_register(app,".mp3","mpg123 -q %C")==0 && launcher_kind("s.mp3")==LAUNCHER_EXTERNAL);
    CHECK(launcher_register(app,".pdf","archive")==0 && launcher_kind("d.pdf")==LAUNCHER_ARCHIVE);
    CHECK(launcher_register(app,".gz","zcat")==0 && launcher_kind("a.tar.gz")==LAUNCHER_ARCHIVE && launcher_kind("x.gz")==LAUNCHER_EXTERNAL);
    CHECK(launcher_count()==base+4);
    const char *e,*c; int found=0;
    for(size_t i=0;!launcher_entry(i,&e,&c);++i) if(!strcmp(e,".mp3")) found=!strcmp(c,"mpg123 -q %C");
    CHECK(found && launcher_entry(launcher_count(),&e,&c)==-1);
    last=NULL; CHECK(launcher_register(app,"","textview")==-1 && launcher_register(app,".x","")==-1 && launcher_register(app,"*","textview")==-1);
    char path[2048];
    snprintf(path,sizeof(path),"%s/it's here.mp3",dir); opened=NULL;
    CHECK(launcher_open(app,path)==0);
    char want[4096]; snprintf(want,sizeof(want),"/bin/sh|-c|mpg123 -q '%s/it'\\''s here.mp3'",dir);
    CHECK(!strcmp(external,want) && !strcmp(cwd,dir));
    if(strcmp(external,want)) fprintf(stderr,"external: %s\n",external);
    launcher_register(app,".mp3","play"); snprintf(want,sizeof(want),"/bin/sh|-c|play '%s/it'\\''s here.mp3'",dir);
    CHECK(launcher_open(app,path)==0 && !strcmp(external,want));
    snprintf(path,sizeof(path),"%s/n.txt",dir); CHECK(launcher_open(app,path)==0 && opened && !strcmp(opened,"noveledit_create"));
    snprintf(path,sizeof(path),"%s/a.tgz",dir); CHECK(launcher_open(app,path)==0 && !strcmp(opened,"archive_browse"));
    snprintf(path,sizeof(path),"%s/p.png",dir); CHECK(launcher_open(app,path)==0 && !strcmp(opened,"imageview_create"));
    snprintf(path,sizeof(path),"%s/s.xlsx",dir); CHECK(launcher_open(app,path)==0 && !strcmp(opened,"sheetview_create"));
    snprintf(path,sizeof(path),"%s/w.docx",dir); CHECK(launcher_open(app,path)==0 && !strcmp(opened,"docview_create"));
    snprintf(path,sizeof(path),"%s/k.pptx",dir); CHECK(launcher_open(app,path)==0 && !strcmp(opened,"slideview_create"));
    snprintf(path,sizeof(path),"%s/plain",dir); CHECK(launcher_open(app,path)==0 && !strcmp(opened,"textview_create"));
    last=NULL; CHECK(launcher_open(app,dir)==-1 && last && !strcmp(last,"ディレクトリです"));
    snprintf(path,sizeof(path),"%s/missing.txt",dir); CHECK(launcher_open(app,path)==-1);
    launcher_reset(); CHECK(launcher_count()==base && launcher_kind("n.txt")==LAUNCHER_TEXT && launcher_kind("d.pdf")==LAUNCHER_PDF);
    snprintf(path,sizeof(path),"%s/d.pdf",dir); CHECK(launcher_open(app,path)==0 && !strcmp(opened,"pdfview_create"));
    if(!fails) printf("launcher unit checks passed\n");
    return fails!=0;
}
'''


def launcher_unit(build):
    directory = build / 'archive-fixture' / 'launcher'
    directory.mkdir(parents=True)
    for name in ("it's here.mp3", 'n.txt', 'a.tgz', 'p.png', 's.xlsx', 'w.docx', 'k.pptx', 'plain', 'd.pdf'):
        (directory / name).write_text('x')
    source = directory / 'launcher_harness.c'
    source.write_text(LAUNCHER_HARNESS)
    exe = directory / 'launcher-harness'
    cc = shlex.split(os.environ.get('CC', 'cc'))
    subprocess.run(cc + ['-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'), '-I' + str(build),
                         str(source), str(build / 'launcher.o'), str(build / 'i18n.o'), '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(directory.resolve())], check=True,
                   env=dict(os.environ, FDTP_LANGUAGE='ja'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    build = args.build.resolve()
    (build / 'tmp').mkdir(parents=True, exist_ok=True)
    for tool in ('tar', 'zip', 'unzip'):
        require(shutil.which(tool), f'{tool} is needed for this check')
    a = Acceptance(build)
    a.browse()
    a.unpack()
    a.pack()
    a.pack_add()
    a.pack_progress()
    a.bad()
    lha = a.lha()
    launcher_unit(build)
    print(f'1b archive checks passed: {a.scripts} scripts, {a.snapshots} snapshots, file system assertions'
          f'{"" if lha else " (LHa skipped)"}, launcher unit checks')


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError, subprocess.CalledProcessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
