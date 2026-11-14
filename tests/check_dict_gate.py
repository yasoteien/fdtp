#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Exercise the dictionary gate using synthetic mount, sysfs, and JDS files."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
NAMES = ('kr', 'mk', 'ej', 'je')
MOUNT = '36 25 7:0 / /mnt/pomera-dict ro,relatime - ext4 /dev/loop0 ro,noload\n'


def put(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding='ascii')


def setup(base):
    shutil.rmtree(base, ignore_errors=True)
    put(base / 'proc/self/mountinfo', MOUNT)
    loop = base / 'sys/block/loop0'
    for name, value in {'dev': '7:0\n', 'ro': '1\n',
                        'loop/backing_file': '/dev/mmcblk0\n',
                        'loop/offset': '639631360\n',
                        'loop/sizelimit': '268435456\n'}.items():
        put(loop / name, value)
    jds = base / 'mnt/pomera-dict/JDS'
    jds.mkdir(parents=True)
    for name in NAMES:
        (jds / (name + '.jds')).write_bytes(b'SDSJ' + struct.pack('<I', 8))
    return jds


def compile_gate(out, fixture=None):
    harness = out.parent / 'gate_harness.c'
    # dict_gate_open must open exactly the books dict_gate_ok passes, and
    # nothing for a value that is not one book's bit.
    harness.write_text('#include <stdio.h>\n#include <unistd.h>\n#include "dict_gate.h"\n'
                       'int main(void) { unsigned ok = dict_gate_ok(), opened = 0;\n'
                       '  for (unsigned i = 0; i < 4; ++i) { int fd = dict_gate_open(1u << i);\n'
                       '    if (fd >= 0) { opened |= 1u << i; close(fd); } }\n'
                       '  int odd = dict_gate_open(3) >= 0 || dict_gate_open(0) >= 0 || dict_gate_open(16) >= 0;\n'
                       '  if (opened != ok || odd) printf("open %u odd %d ", opened, odd);\n'
                       '  printf("%u\\n", ok); return 0; }\n')
    cmd = [os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra',
           '-I', str(ROOT / 'src')]
    if fixture is not None:
        cmd.append('-DDICT_GATE_TEST_ROOT="' + str(fixture) + '"')
    subprocess.run(cmd + [str(ROOT / 'src/dict_gate.c'), str(harness), '-o', str(out)], check=True)


def check(exe, expected, label):
    result = subprocess.check_output([str(exe)], text=True).strip()
    assert result == str(expected), f'{label}: expected {expected}, got {result}'


def main():
    with tempfile.TemporaryDirectory(prefix='fdtp-dict-gate-') as tmp:
        base = Path(tmp) / 'fixture'
        exe = Path(tmp) / 'gate'
        compile_gate(exe, base)
        jds = setup(base)
        check(exe, 15, 'all four')
        for index, name in enumerate(NAMES):
            jds = setup(base)
            (jds / (name + '.jds')).unlink()
            check(exe, 15 ^ (1 << index), 'missing ' + name)
        jds = setup(base)
        for name in NAMES[1:]:
            (jds / (name + '.jds')).unlink()
        check(exe, 1, 'one dictionary')
        (jds / 'kr.jds').unlink()
        check(exe, 0, 'no dictionaries')

        for label, mount in {
            'wrong mountpoint': MOUNT.replace('/mnt/pomera-dict ', '/mnt/pomera-dict2 '),
            'bind subdirectory': MOUNT.replace('7:0 / /mnt/', '7:0 /subdir /mnt/'),
            'writable mount': MOUNT.replace('ro,relatime', 'rw,relatime'),
            'writable superblock': MOUNT.replace('ro,noload', 'rw,noload'),
            'missing noload': MOUNT.replace('ro,noload', 'ro'),
            'wrong filesystem': MOUNT.replace(' ext4 ', ' ext3 '),
            'wrong source': MOUNT.replace('/dev/loop0', '/dev/mmcblk0'),
            'malformed source': MOUNT.replace('/dev/loop0', '/dev/loop0extra'),
            'wrong device number': MOUNT.replace('7:0', '7:1'),
        }.items():
            setup(base)
            put(base / 'proc/self/mountinfo', mount)
            check(exe, 0, label)
        setup(base)
        put(base / 'proc/self/mountinfo', MOUNT.replace('noload', 'norecovery'))
        check(exe, 15, 'norecovery alias')
        setup(base)
        (base / 'proc/self/mountinfo').unlink()
        check(exe, 0, 'missing mountinfo')

        for field, bad in {'dev': '7:1\n', 'ro': '0\n',
                           'loop/backing_file': '/dev/mmcblk1\n',
                           'loop/offset': '639631361\n',
                           'loop/sizelimit': '268435455\n'}.items():
            setup(base)
            path = base / 'sys/block/loop0' / field
            put(path, bad)
            check(exe, 0, 'wrong sysfs ' + field)
            path.unlink()
            check(exe, 0, 'missing sysfs ' + field)

        for label, contents in {
            'empty': b'', 'short magic': b'SDS', 'short header': b'SDSJ\x08\x00\x00',
            'wrong magic': b'XXXX' + struct.pack('<I', 8),
            'size too small': b'SDSJ' + struct.pack('<I', 7),
            'size too large': b'SDSJ' + struct.pack('<I', 9),
            'big endian length': b'SDSJ' + struct.pack('>I', 8),
            'trailing data': b'SDSJ' + struct.pack('<I', 8) + b'X',
        }.items():
            jds = setup(base)
            (jds / 'kr.jds').write_bytes(contents)
            check(exe, 14, label)
        jds = setup(base)
        kr = jds / 'kr.jds'
        kr.unlink()
        kr.symlink_to(jds / 'mk.jds')
        check(exe, 14, 'symlink')
        kr.unlink()
        kr.mkdir()
        check(exe, 14, 'directory')

        # The ATOK28 files beside the books (dict_gate_open_atok).
        atok_exe = Path(tmp) / 'gate_atok'
        atok_src = Path(tmp) / 'gate_atok.c'
        atok_src.write_text('#include <stdio.h>\n#include <unistd.h>\n#include "dict_gate.h"\n'
                            'int main(void) { unsigned bits = 0;\n'
                            '  for (unsigned i = 0; i <= DICT_GATE_ATOK_COUNT; ++i) { int fd = dict_gate_open_atok(i);\n'
                            '    if (fd >= 0) { bits |= 1u << i; close(fd); } }\n'
                            '  printf("%u\\n", bits); return 0; }\n')
        cmd = [os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra', '-I', str(ROOT / 'src'),
               '-DDICT_GATE_TEST_ROOT="' + str(base) + '"']
        subprocess.run(cmd + [str(ROOT / 'src/dict_gate.c'), str(atok_src), '-o', str(atok_exe)], check=True)
        atok_names = ('ATEX_SPORTS.DIC', 'ATEX_ANIME.DIC', 'ATEX_ENTERTAINMENT.DIC', 'ATOK28NM.DIC')

        def setup_atok():
            setup(base)
            folder = base / 'mnt/pomera-dict/ATOK/share/atok28'
            folder.mkdir(parents=True)
            for name in atok_names:
                (folder / name).write_bytes(b'DIC\0\0\0\0\0ATOK28\0\0' + bytes(20))
            return folder
        folder = setup_atok()
        check(atok_exe, 15, 'atok: all four')
        (folder / 'ATEX_ANIME.DIC').write_bytes(b'XIC\0\0\0\0\0ATOK28\0\0')
        check(atok_exe, 13, 'atok: wrong head')
        (folder / 'ATEX_ANIME.DIC').unlink()
        (folder / 'ATEX_ANIME.DIC').symlink_to(folder / 'ATEX_SPORTS.DIC')
        check(atok_exe, 13, 'atok: symlink')
        setup_atok()
        put(base / 'proc/self/mountinfo', MOUNT.replace('ro,noload', 'ro'))
        check(atok_exe, 0, 'atok: mount check')
        folder = setup_atok()
        shutil.rmtree(base / 'mnt/pomera-dict/ATOK/share')
        (base / 'mnt/pomera-dict/ATOK/share').symlink_to(base)
        check(atok_exe, 0, 'atok: symlinked folder')

        if sys.platform == 'darwin':
            stub = Path(tmp) / 'stub'
            compile_gate(stub)
            check(stub, 0, 'macOS production stub')
    print('dict gate: ok')


if __name__ == '__main__':
    main()
