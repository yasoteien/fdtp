#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Revision 1 D: the LHa archives made by p (src/lha_write.c, src/lha_lh5.c).

Run from the repository root after the build:
    python3 -B tests/check_lha.py --build build
Builds tests/lha_harness.c twice (plain, and with ASan/UBSan), makes
archives of a fixture tree under BUILD/lha-fixture and checks them with
Lhasa (`lha` / `lhasa`: t, p, x) and tests/lha_read.py (headers): the
bytes, names (Shift_JIS and UTF-8), times, modes, directories, links,
-lh0- for data that does not shrink, the level 2 padding byte, levels,
step sizes, and adding to an archive. Nothing is fetched from the network.
"""
import argparse
import json
import os
from pathlib import Path
import random
import re
import shlex
import shutil
import subprocess
import sys
import zlib

from check import require
from lha_read import lha_contents, lha_read

ROOT = Path(__file__).resolve().parents[1]


def mk_sources():
    text = (ROOT / 'mk' / 'lha.mk').read_text()
    return re.search(r'^LHA_SOURCES\s*:=\s*(.+)$', text, re.M).group(1).split()


def build_harness(build, sanitize):
    exe = build / ('lha-harness-asan' if sanitize else 'lha-harness')
    sources = [ROOT / 'src' / f'{n}.c' for n in mk_sources() + ['jis', 'i18n']] + [ROOT / 'tests' / 'lha_harness.c']
    cc = shlex.split(os.environ.get('CC', 'cc'))
    flags = ['-std=c11', '-D_DEFAULT_SOURCE', '-D_FILE_OFFSET_BITS=64', '-Wall', '-Wextra', '-Werror', '-g', '-O1',
             '-I' + str(ROOT / 'src'), '-I' + str(build)]
    if sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=undefined']
    subprocess.run(cc + flags + [str(s) for s in sources] + ['-o', str(exe)], check=True)
    return exe


class Lha:
    def __init__(self, build, base, sanitize):
        self.exe = build_harness(build, sanitize)
        self.base = base
        self.lha = shutil.which('lhasa') or shutil.which('lha')
        self.runs = 0

    def make(self, dest, src_dir, names, *flags, fail=False):
        r = subprocess.run([str(self.exe), *flags, str(dest), str(src_dir), *names], capture_output=True, text=True,
                           env=dict(os.environ, FDTP_LANGUAGE='ja', TZ='UTC'))
        self.runs += 1
        require(r.stderr == '', f'harness wrote to stderr (sanitizer?): {r.stderr[:2000]}')
        out = json.loads(r.stdout)
        if fail:
            require(r.returncode != 0 and out['result'] != 0, f'expected a failure: {out}')
        else:
            require(r.returncode == 0 and out['result'] == 0, f'{dest.name}: {out}')
        return out

    def test(self, archive):
        r = subprocess.run([self.lha, 't', str(archive)], capture_output=True)
        require(r.returncode == 0, f'lha t {archive.name}: {r.stdout[-400:]!r} {r.stderr[-400:]!r}')

    def extract(self, archive, into):
        if into.exists():
            shutil.rmtree(into)
        into.mkdir(parents=True)
        subprocess.run([self.lha, 'xq', str(archive.resolve())], check=True, cwd=into)
        return into


def same_tree(src, dst, what, dir_times=True):
    for path in sorted([src, *src.rglob('*')]):
        rel = path.relative_to(src)
        other = dst / rel
        require(other.exists() or other.is_symlink(), f'{what}: {rel} missing')
        if path.is_symlink():
            require(other.is_symlink() and os.readlink(other) == os.readlink(path), f'{what}: link {rel}')
            continue
        st, ot = path.stat(), other.stat()
        require((st.st_mode & 0o777) == (ot.st_mode & 0o777), f'{what}: mode of {rel} {oct(st.st_mode)} {oct(ot.st_mode)}')
        if path.is_dir():
            require(other.is_dir(), f'{what}: directory {rel}')
            if dir_times:
                require(int(ot.st_mtime) == int(st.st_mtime), f'{what}: time of directory {rel}')
        else:
            require(other.read_bytes() == path.read_bytes(), f'{what}: bytes of {rel}')
            require(int(ot.st_mtime) == int(st.st_mtime), f'{what}: time of {rel}')
    extra = [p for p in dst.rglob('*') if not ((src / p.relative_to(dst)).exists() or (src / p.relative_to(dst)).is_symlink())]
    require(not extra, f'{what}: unexpected {extra}')


def stamp(tree, t=1600000000):
    for i, p in enumerate(sorted([tree, *tree.rglob('*')], key=lambda p: -len(p.parts))):
        os.utime(p, (t, t + i * 7), follow_symlinks=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    build = args.build.resolve()
    require((build / 'jis0208.inc').exists(), 'build the program first (jis0208.inc)')
    if not (shutil.which('lhasa') or shutil.which('lha')):
        print('LHa checks skipped: no lha (Lhasa) on this machine')
        return
    base = build / 'lha-fixture'
    if base.exists():
        shutil.rmtree(base)
    base.mkdir(parents=True)
    rng = random.Random(1974)
    report = []

    # ---- the fixture ----
    src = base / 'src'
    tree = src / 'tree'
    (tree / 'sub' / '深い階層' / 'a' / 'b' / 'c' / 'd' / 'e').mkdir(parents=True)
    (tree / 'empty dir').mkdir()
    (tree / '日本語の名前.txt').write_text('日本語の中身\n')
    (tree / 'ｶﾀｶﾅ.txt').write_bytes(b'half-width katakana\n')
    (tree / '表ソ能.txt').write_bytes(b'0x5C as the second byte in Shift_JIS\n')
    (tree / 'empty.txt').write_bytes(b'')
    (tree / 'one').write_bytes(b'1')
    (tree / 'sub' / '深い階層' / 'a' / 'b' / 'c' / 'd' / 'e' / 'deep.txt').write_text('deep\n')
    (tree / 'sub' / 'link').symlink_to('../日本語の名前.txt')
    (tree / 'exec.sh').write_text('#!/bin/sh\n')
    os.chmod(tree / 'exec.sh', 0o755)
    os.chmod(tree / 'empty dir', 0o700)
    big = src / 'big'
    big.mkdir()
    (big / 'random.bin').write_bytes(rng.randbytes(4 * 1024 * 1024))
    (big / 'repeat.txt').write_bytes(b''.join(b'%06d repeated record with the same words, ' % (i % 1500) + b'x' * (i % 13) + b'\n' for i in range(100000)))
    (big / 'zeros.bin').write_bytes(bytes(3 * 1024 * 1024))
    edges = src / 'edges'
    edges.mkdir()
    for n in (1, 2, 3, 255, 256, 257, 65535, 65536, 65537):
        (edges / f'text{n}').write_bytes((b'abcabcabd' * (n // 9 + 1))[:n])
    (edges / 'random524288').write_bytes(rng.randbytes(524288))
    (edges / 'random524289').write_bytes(rng.randbytes(524289))
    for period in (8190, 8191, 8192, 8193):
        block = rng.randbytes(period)
        (edges / f'period{period}').write_bytes(block * 6)
    twin = rng.randbytes(3000) * 5
    (edges / 'twin1').write_bytes(twin)
    (edges / 'twin2').write_bytes(twin)
    (edges / 'run10000').write_bytes(b'a' * 10000 + b'b' * 256 + b'c' * 257 + b'd' * 3)
    (edges / 'source.c').write_bytes(b''.join(p.read_bytes() for p in sorted((ROOT / 'src').glob('*.c'))[:12]))
    # Level 2 headers of 255, 256 (padded to 257) and 257 bytes: 39 bytes,
    # 7 for the directory "pad", and the name.
    pad = src / 'pad'
    pad.mkdir()
    for n in (209, 210, 211):
        (pad / ('p' * (n - 4) + '.txt')).write_bytes(b'pad\n')
    for d in (tree, big, edges, pad):
        stamp(d)
    os.utime(tree / 'sub' / 'link', (1600000000, 1600000000), follow_symlinks=False)

    for sanitize in (False, True):
        tag = 'asan' if sanitize else 'plain'
        lha = Lha(build, base, sanitize)
        out = base / tag
        out.mkdir()

        # UTF-8 names: Lhasa unpacks them on the Mac; the whole tree must match.
        r = lha.make(out / 'tree-utf8.lzh', src, ['tree'], '-u')
        lha.test(out / 'tree-utf8.lzh')
        members = lha_read(out / 'tree-utf8.lzh')
        require(all(m['level'] == 2 and m['os'] == 'U' and m['header_crc_ok'] for m in members), 'level 2, UNIX, header CRC')
        byname = {m['path']: m for m in members}
        require(byname['tree']['method'] == '-lhd-' and byname['tree/empty dir']['method'] == '-lhd-', 'directories are -lhd-')
        require(byname['tree/sub/link']['link'] == '../日本語の名前.txt' and (byname['tree/sub/link']['mode'] & 0o170000) == 0o120000, 'the link (LHa for UNIX form)')
        require(byname['tree/empty.txt']['method'] == '-lh0-' and byname['tree/empty.txt']['size'] == 0, 'an empty file is stored')
        require(byname['tree/one']['method'] == '-lh0-', 'one byte does not shrink: stored')
        require(byname['tree/exec.sh']['mode'] == 0o100755 and byname['tree/empty dir']['mode'] == 0o40700, 'modes')
        require(byname['tree/sub/深い階層/a/b/c/d/e/deep.txt']['mtime'] == int((tree / 'sub/深い階層/a/b/c/d/e/deep.txt').stat().st_mtime), 'time in the header')
        require(r['added'] == len(members) and r['stored'] >= 2, f'counts: {r}')
        x = lha.extract(out / 'tree-utf8.lzh', out / 'x-tree')
        # Lhasa makes links last, which touches the time of their directory.
        same_tree(tree, x / 'tree', 'tree (UTF-8)', dir_times=False)
        for d in [tree, *[p for p in tree.rglob('*') if p.is_dir() and not p.is_symlink()]]:
            if d != tree / 'sub':
                require(int((x / d.relative_to(src)).stat().st_mtime) == int(d.stat().st_mtime), f'directory time {d.relative_to(src)}')

        # Shift_JIS names (the default): headers and data; Lhasa cannot make
        # these names on the Mac, so the data is checked through `lha p`.
        lha.make(out / 'tree-sjis.lzh', src, ['tree'])
        lha.test(out / 'tree-sjis.lzh')
        sj = lha_read(out / 'tree-sjis.lzh')
        require([m['path'] for m in sj] == [m['path'] for m in members], 'the same members in Shift_JIS')
        raw = {m['path']: m['raw'] for m in sj}
        require(raw['tree/日本語の名前.txt'] == 'tree/日本語の名前.txt'.encode('cp932'), 'Shift_JIS bytes')
        require(raw['tree/表ソ能.txt'] == 'tree/表ソ能.txt'.encode('cp932') and b'\\' in raw['tree/表ソ能.txt'], 'a 0x5C second byte kept as it is')
        require(raw['tree/ｶﾀｶﾅ.txt'] == 'tree/ｶﾀｶﾅ.txt'.encode('cp932') and len(raw['tree/ｶﾀｶﾅ.txt']) == len('tree/') + 4 + 4, 'half-width katakana in one byte each')
        require(all(m['encoding'] != 'utf-8' for m in sj), 'no UTF-8 in a Shift_JIS archive')
        data = lha_contents(lha.lha, out / 'tree-sjis.lzh', sj)
        require(data == {p: (src / p).read_bytes() for p in data} and len(data) == sum(1 for m in sj if m['method'] != '-lhd-'), 'Shift_JIS archive data')

        # A name Shift_JIS cannot hold: refused before anything is written; UTF-8 takes it.
        (src / 'emoji').mkdir(exist_ok=True)
        (src / 'emoji' / 'face😀.txt').write_text('x')
        bad = lha.make(out / 'emoji.lzh', src, ['emoji'], fail=True)
        require('Shift_JIS にできない名前' in bad['error'] and not (out / 'emoji.lzh').exists(), f'refused: {bad}')
        lha.make(out / 'emoji.lzh', src, ['emoji'], '-u')
        lha.test(out / 'emoji.lzh')

        # Large files: random (stored, as -lh5- would be larger), repeated, zeros.
        r = lha.make(out / 'big.lzh', src, ['big'], '-u', '-s', '1000000000')
        bm = {m['path']: m for m in lha_read(out / 'big.lzh')}
        require(bm['big/random.bin']['method'] == '-lh0-' and bm['big/random.bin']['packed'] == 4 * 1024 * 1024, 'random data stored')
        require(bm['big/repeat.txt']['method'] == '-lh5-' and bm['big/zeros.bin']['method'] == '-lh5-', 'compressible data compressed')
        lha.test(out / 'big.lzh')
        same_tree(big, lha.extract(out / 'big.lzh', out / 'x-big') / 'big', 'big files')

        # Sizes and patterns at the edges (chunk 64 KB, early -lh0- at 512 KB,
        # window 8 KB, matches of 3 to 256 bytes), at levels 1, 5 and 9.
        for level in (1, 5, 9):
            name = f'edges{level}.lzh'
            lha.make(out / name, src, ['edges'], '-u', '-l', str(level))
            lha.test(out / name)
            same_tree(edges, lha.extract(out / name, out / f'x-{name}') / 'edges', name)
            em = {m['path']: m for m in lha_read(out / name)}
            require(em['edges/random524288']['method'] == '-lh0-' and em['edges/random524289']['method'] == '-lh0-', 'random stored at the 512 KB edge')
            require(em['edges/period8190']['packed'] < 8190 * 2 and em['edges/period8191']['packed'] < 8191 * 2, f'periods inside the window compress: {em["edges/period8191"]["packed"]}')
            require(em['edges/run10000']['packed'] < 200, f'long runs: {em["edges/run10000"]["packed"]}')
            # One encoder serves the whole archive; no match may reach into the previous file.
            require(em['edges/twin1']['packed'] == em['edges/twin2']['packed'] < 4000, f'files compressed alone: {em["edges/twin1"]["packed"]} {em["edges/twin2"]["packed"]}')

        # Step sizes do not change the archive.
        a1 = lha.make(out / 'steps-small.lzh', src, ['edges'], '-u', '-s', '4096')
        lha.make(out / 'steps-one.lzh', src, ['edges'], '-u', '-s', '100000000000')
        require((out / 'steps-small.lzh').read_bytes() == (out / 'steps-one.lzh').read_bytes(), 'the same archive in 4 KB steps and in one')
        require(a1['steps'] > 30 and a1['monotonic'], f'progress only grows: {a1["steps"]} {a1["monotonic"]}')

        # The level 2 header whose size has a 0 low byte gets a padding byte.
        lha.make(out / 'pad.lzh', src, ['pad'], '-u')
        lha.test(out / 'pad.lzh')
        sizes = {len(m['path']) - 4: (m['header_size'], m['padded']) for m in lha_read(out / 'pad.lzh') if m['method'] != '-lhd-'}
        require(sizes == {209: (255, False), 210: (257, True), 211: (257, False)}, f'header sizes: {sizes}')
        same_tree(pad, lha.extract(out / 'pad.lzh', out / 'x-pad') / 'pad', 'padded headers')

        # Special files are skipped and counted; an unreadable file stops the job.
        special = src / 'special'
        if not special.exists():
            special.mkdir()
            os.mkfifo(special / 'fifo')
            (special / 'ok.txt').write_text('ok\n')
        r = lha.make(out / 'special.lzh', src, ['special'], '-u')
        require(r['skipped'] == 1 and [m['path'] for m in lha_read(out / 'special.lzh')] == ['special', 'special/ok.txt'], f'fifo skipped: {r}')
        locked = src / 'locked'
        if not locked.exists():
            locked.mkdir()
            (locked / 'secret.txt').write_text('no\n')
        os.chmod(locked / 'secret.txt', 0)
        try:
            r = lha.make(out / 'locked.lzh', src, ['locked'], fail=True)
        finally:
            os.chmod(locked / 'secret.txt', 0o644)
        require('読めません' in r['error'] and not (out / 'locked.lzh').exists() and not list(out.glob('locked.lzh.tmp*')), f'unreadable file: {r}')
        # 4 GB (a sparse file): the 32-bit sizes cannot hold it.
        huge = src / 'huge'
        if not huge.exists():
            huge.mkdir()
            with open(huge / 'four.bin', 'wb') as f:
                f.truncate(4 * 1024 ** 3)
        r = lha.make(out / 'huge.lzh', src, ['huge'], fail=True)
        require('4 GB' in r['error'] and not (out / 'huge.lzh').exists(), f'4 GB refused: {r}')

        # Adding: members kept are copied byte for byte; replaced ones are
        # rewritten in place; new ones go at the end.
        work = base / f'add-{tag}'
        shutil.copytree(tree, work / 'tree', symlinks=True)
        stamp(work / 'tree')
        lha.make(out / 'add.lzh', work, ['tree'], '-u')
        before = (out / 'add.lzh').read_bytes()
        old = {m['path']: m for m in lha_read(out / 'add.lzh')}
        shutil.copy(out / 'add.lzh', out / 'add-newer.lzh')
        (work / 'tree' / '日本語の名前.txt').write_text('changed, dated earlier\n')
        os.utime(work / 'tree' / '日本語の名前.txt', (1500000000, 1500000000))
        (work / 'tree' / 'one').write_text('changed later\n')
        os.utime(work / 'tree' / 'one', (1700000000, 1700000000))
        (work / 'tree' / 'added.txt').write_text('new\n')
        os.utime(work / 'tree' / 'added.txt', (1700000000, 1700000000))
        os.utime(work / 'tree', (1600000000, 1600000000))
        r = lha.make(out / 'add.lzh', work, ['tree/日本語の名前.txt', 'tree/one', 'tree/added.txt'], '-u', '-a')
        after = lha_read(out / 'add.lzh')
        paths = [m['path'] for m in after]
        require(paths == list(old) + ['tree/added.txt'], f'order kept, the new at the end: {paths}')
        new_bytes = (out / 'add.lzh').read_bytes()
        for m in after:
            if m['path'] in ('tree/日本語の名前.txt', 'tree/one', 'tree/added.txt'):
                continue
            o = old[m['path']]
            require(new_bytes[m['offset']:m['offset'] + m['length']] == before[o['offset']:o['offset'] + o['length']], f'kept byte for byte: {m["path"]}')
        require(r['added'] == 3 and r['kept'] == len(old) - 2 and r['older'] == 0, f'add counts: {r}')
        lha.test(out / 'add.lzh')
        got = lha_contents(lha.lha, out / 'add.lzh', after)
        require(got['tree/日本語の名前.txt'] == b'changed, dated earlier\n' and got['tree/one'] == b'changed later\n' and got['tree/added.txt'] == b'new\n', 'replaced unconditionally')
        r = lha.make(out / 'add-newer.lzh', work, ['tree/日本語の名前.txt', 'tree/one', 'tree/added.txt'], '-u', '-a', '-n')
        got = lha_contents(lha.lha, out / 'add-newer.lzh', lha_read(out / 'add-newer.lzh'))
        require(got['tree/日本語の名前.txt'] == '日本語の中身\n'.encode() and got['tree/one'] == b'changed later\n' and got['tree/added.txt'] == b'new\n', 'newer only')
        require(r['older'] == 1 and r['added'] == 2, f'newer-only counts: {r}')
        # Adding to an archive made elsewhere (level 0, DOS names with '\').
        legacy = out / 'legacy.lzh'
        legacy.write_bytes(lzh_level0([('OLD\\A.TXT', b'old a\n'), ('B.TXT', b'old b\n')]))
        legacy_before = legacy.read_bytes()
        (work / 'B.TXT').write_text('new b\n')
        lha.make(legacy, work, ['B.TXT'], '-u', '-a')
        lm = lha_read(legacy)
        require([m['path'] for m in lm] == ['OLD/A.TXT', 'B.TXT'] and lm[0]['level'] == 0 and lm[1]['level'] == 2, f'level 0 kept, B replaced: {lm}')
        require(legacy.read_bytes()[:lm[0]['length']] == legacy_before[:lm[0]['length']], 'level 0 member copied')
        lha.test(legacy)
        # A broken archive is left as it was.
        broken = out / 'broken.lzh'
        broken.write_bytes(before[:len(before) // 2])
        r = lha.make(broken, work, ['tree/one'], '-u', '-a', fail=True)
        require('壊れています' in r['error'] and broken.read_bytes() == before[:len(before) // 2], f'broken archive untouched: {r}')
        require(not list(out.glob('*.tmp*')), f'temporary files left: {list(out.glob("*.tmp*"))}')

        # Varied data at random levels, compared through lha p.
        fuzz = base / f'fuzz-{tag}'
        fuzz.mkdir()
        for i in range(24):
            parts = []
            for _ in range(rng.randrange(1, 8)):
                kind = rng.randrange(4)
                n = rng.choice((1, 2, 3, 100, 4095, 8191, 8192, 8193, 30000, 70000))
                if kind == 0:
                    parts.append(rng.randbytes(n))
                elif kind == 1:
                    parts.append(bytes([rng.randrange(256)]) * n)
                elif kind == 2:
                    seed = rng.randbytes(rng.randrange(1, 300))
                    parts.append((seed * (n // len(seed) + 1))[:n])
                else:
                    parts.append(bytes(rng.choice(b'ACGT') for _ in range(n)))
            (fuzz / f'f{i:02d}').write_bytes(b''.join(parts))
        for level in (2, 4, 6, 8):
            name = f'fuzz{level}.lzh'
            lha.make(out / name, base, [fuzz.name], '-u', '-l', str(level), '-s', str(rng.choice((1, 999, 65536, 300000))))
            lha.test(out / name)
            fm = lha_read(out / name)
            got = lha_contents(lha.lha, out / name, fm)
            require(all(got[p] == (base / p).read_bytes() for p in got) and len(got) == 24, f'{name}: data')

        if not sanitize:
            # Compression for the report: -lh5- at levels 1/5/9 against gzip -9.
            for label, path in (('repeat.txt', big / 'repeat.txt'), ('source.c', edges / 'source.c')):
                row = [label, path.stat().st_size]
                for level in (1, 5, 9):
                    lha.make(out / f'ratio{level}.lzh', path.parent, [path.name], '-u', '-l', str(level))
                    row.append({m['path']: m for m in lha_read(out / f'ratio{level}.lzh')}[path.name]['packed'])
                row.append(len(zlib.compress(path.read_bytes(), 9)) - 6)
                report.append(row)
        print(f'  {tag}: {lha.runs} archives made and checked')

    for name, size, l1, l5, l9, gz in report:
        print(f'  {name}: {size} bytes -> -lh5- level 1 {l1} ({l1 * 100 / size:.1f}%), 5 {l5} ({l5 * 100 / size:.1f}%), '
              f'9 {l9} ({l9 * 100 / size:.1f}%); deflate -9 {gz} ({gz * 100 / size:.1f}%)')
    print('revision 1 D LHa checks passed: plain and ASan/UBSan harnesses, Lhasa t/p/x, headers, adding')


def lzh_level0(entries):
    import struct
    import time
    from lha_read import crc16

    def dos_time(t):
        lt = time.localtime(t)
        return ((lt.tm_year - 1980) << 25) | (lt.tm_mon << 21) | (lt.tm_mday << 16) | (lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec // 2)
    out = b''
    for name, data in entries:
        n = name.encode()
        body = b'-lh0-' + struct.pack('<IIIBBB', len(data), len(data), dos_time(1600000000), 0x20, 0, len(n)) + n + struct.pack('<H', crc16(data))
        out += bytes([len(body), sum(body) & 0xff]) + body + data
    return out + b'\0'


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError, subprocess.CalledProcessError, ValueError, KeyError) as error:
        print(f'FAIL: {error!r}', file=sys.stderr)
        sys.exit(1)
