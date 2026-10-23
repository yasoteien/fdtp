#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Revision 1 item 31: the system information (Alt+i).

    python3 -B tests/check_sysinfo.py --build build [--png]

Alt+i opens it from the filer, in the frame of every full screen; any key
goes back. On the Mac (no /proc, /sys) every value that cannot be read says
「ありません」 and nothing breaks. With FDTP_SYSINFO_ROOT a made-up tree of
/proc, /sys, /etc and /dev is read: the values, the GPU's time at its
boundary (899999 ms yellow with how to change it, 900000 not, -1 never
sleeps), a file without permission, the key devices, a battery or none.
"""
import argparse
import os
from pathlib import Path
import shutil
import sys

from check_wide import Runner, require
import check_wide

ROOT = Path(__file__).resolve().parents[1]
WHITE, YELLOW, CYAN = 2, 3, 1
VERSION = (ROOT / 'src' / 'version.h').read_text().split('FDTP_VERSION "')[1].split('"')[0]


def rows_of(s):
    """{label: (value, colour of its first character)} from the frame's body."""
    out, last = {}, None
    for y in range(1, s.nrows - 3):
        label, value = s.text(y, 2, 17).strip(), s.text(y, 20, s.cols - 1).strip()
        if not value:
            continue
        colour = next(c[1] for c in s.rows[y][20:] if c[0] != 32)
        key = label or (last + '+')
        out[key] = (value, colour)
        last = label or last
    return out


DEVICES = '''I: Bus=0019 Vendor=0001 Product=0001 Version=0100
N: Name="gpio-keys"
P: Phys=gpio-keys/input0
H: Handlers=kbd event1

I: Bus=0003 Vendor=0fac Product=0ade Version=0001
N: Name="keyd virtual keyboard"
H: Handlers=sysrq kbd leds event4

'''


def make_root(base, gpu, battery=True, readable=True):
    root = base / 'sysroot'
    shutil.rmtree(root, ignore_errors=True)
    for d in ('proc/bus/input', 'etc', 'dev/input', 'sys/devices/platform/10090000.gpu/power', 'sys/class/power_supply'):
        (root / d).mkdir(parents=True)
    (root / 'etc/debian_version').write_text('13.1\n')
    (root / 'proc/uptime').write_text('183845.25 360000.00\n')   # 2 日 3 時間 4 分
    (root / 'proc/meminfo').write_text('MemTotal:        1017856 kB\nMemFree:          100000 kB\nMemAvailable:     655360 kB\n')
    (root / 'proc/bus/input/devices').write_text(DEVICES)
    (root / 'dev/input/event1').write_text('')
    event4 = root / 'dev/input/event4'
    event4.write_text('')
    if not readable:
        event4.chmod(0)
    gpu_file = root / 'sys/devices/platform/10090000.gpu/power/autosuspend_delay_ms'
    gpu_file.write_text(f'{gpu}\n')
    if gpu == 'unreadable':
        gpu_file.write_text('200\n')
        gpu_file.chmod(0)
    if battery:
        (root / 'sys/class/power_supply/AC').mkdir()
        (root / 'sys/class/power_supply/AC/online').write_text('1\n')
        (root / 'sys/class/power_supply/battery').mkdir()
        (root / 'sys/class/power_supply/battery/capacity').write_text('85\n')
        (root / 'sys/class/power_supply/battery/status').write_text('Discharging\n')
    return root


def check_frame(s, label):
    cols, rows = s.cols, s.nrows
    require(s.boxes() and s.boxes()[0][:4] == [0, 0, cols, rows - 2] and s.boxes()[0][5] == 1, f'{label}: the screen\'s frame {s.boxes()}')
    require('システム情報' in s.text(0), f'{label}: the title {s.text(0)!r}')
    require('何かキー' in s.text(rows - 1) and '戻る' in s.text(rows - 1), f'{label}: the band {s.text(rows - 1)!r}')
    last = max(y for y in range(1, rows - 3) if s.text(y, 1, cols - 1).strip())
    require(last <= rows - 4, f'{label}: on one screen (down to row {last})')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--png', action='store_true')
    args = parser.parse_args()
    build = args.build.resolve()
    (build / 'tmp').mkdir(parents=True, exist_ok=True)
    r = Runner(build, args.png)
    r.base = build / 'sysinfo-check'
    r.env = dict(r.env, HOME=str(r.base / 'home'), FDTP_RC_USER=str(r.base / 'none.fdtprc'))
    shutil.rmtree(r.base / 'snaps', ignore_errors=True)
    d = r.base / 'files'
    d.mkdir(parents=True, exist_ok=True)
    (d / 'a.txt').write_text('a\n')

    # Alt+i from the filer opens it; any key goes back to the filer.
    s = r.run('open', 'SNAP M-i SNAP x SNAP', d, ['--size', '80x25'], env={'FDTP_SYSINFO_ROOT': str(r.base / 'nothing')}, expect=3)
    require(s[0].data['depth'] == 1 and s[1].data['depth'] == 2 and s[2].data['depth'] == 1, 'Alt+i opens it, a key goes back')
    require(s[2].rows == s[0].rows, 'back to the same filer')
    # Nothing to read (the Mac): every value says why, nothing breaks.
    v = rows_of(s[1])
    check_frame(s[1], 'nothing')
    require(v['FDTP'][0] == VERSION and v['Debian'][0] == 'ありません' and v['起動してから'][0] == 'ありません', f'nothing: {v}')
    require(v['キーの装置'][0] == 'ありません' and v['GPU の休むまで'][0] == 'ありません' and v['メモリ'][0] == 'ありません', f'nothing: {v}')
    require('電池' not in v and v['画面'][0].startswith('80x25（絵 1024x600）'), f'nothing: no battery, the screen {v}')
    require('ディスクの空き' in v and ('GB' in v['ディスクの空き'][0] or 'MB' in v['ディスクの空き'][0]), f'the free space {v}')
    if sys.platform == 'darwin':
        # The real root of the Mac: no /proc, no /sys either.
        s = r.run('mac', 'M-i SNAP', d, ['--size', '85x25'], env={'FDTP_SYSINFO_ROOT': ''}, expect=1)
        check_frame(s[0], 'mac')
        require(rows_of(s[0])['GPU の休むまで'][0] == 'ありません', 'mac: no GPU file')

    # A made-up tree: the values and the GPU's boundary.
    cases = [(899999, YELLOW, True, '899999 ms（900.0 秒）'), (900000, WHITE, False, '900000 ms（15 分）'),
             (200, YELLOW, True, '200 ms（0.2 秒）'), (-1, WHITE, False, '-1 ms（休まない）')]
    for gpu, colour, warned, text in cases:
        root = make_root(r.base, gpu)
        for size in ('80x25', '85x25', '160x45'):
            s = r.run(f'gpu{gpu}-{size}', 'M-i SNAP', d, ['--size', size], env={'FDTP_SYSINFO_ROOT': str(root)}, expect=1)[0]
            check_frame(s, f'gpu {gpu} {size}')
            v = rows_of(s)
            require(v['GPU の休むまで'] == (text, colour), f'gpu {gpu}: {v["GPU の休むまで"]}')
            require(('GPU の休むまで+' in v) == warned, f'gpu {gpu}: the warning {v.get("GPU の休むまで+")}')
            if warned:
                require(v['GPU の休むまで+'] == ('15 分より短い。変え方: 運用マニュアル §8（sudo）', YELLOW), v['GPU の休むまで+'])
            require(v['Debian'][0] == '13.1' and v['起動してから'][0] == '2 日 3 時間 4 分', f'versions {v}')
            require(v['メモリ'][0] == '使える 640 MB／全体 994 MB', f'memory {v["メモリ"]}')
            require(v['キーの装置'][0] == 'keyd: 有り（読める）  gpio-keys: 有り（読める）', f'keys {v["キーの装置"]}')
            require(v['電池'][0] == '85%・放電中', f'battery {v["電池"]}')
            require(v['AWAKE_MINUTES'][0] == '15 分', f'awake {v["AWAKE_MINUTES"]}')
    # No permission (when not root), no battery, keyd's device not readable.
    root = make_root(r.base, 'unreadable', battery=False, readable=False)
    s = r.run('unreadable', 'M-i SNAP', d, ['--size', '85x25'], env={'FDTP_SYSINFO_ROOT': str(root)}, expect=1)[0]
    v = rows_of(s)
    if os.geteuid() != 0:
        require(v['GPU の休むまで'][0] == '読めません（権限）', f'unreadable: {v["GPU の休むまで"]}')
        require(v['キーの装置'][0] == 'keyd: 有り（読めません）  gpio-keys: 有り（読める）', f'unreadable keys: {v["キーの装置"]}')
    require('電池' not in v, 'no battery: no row')
    # AWAKE_MINUTES=0 from ~/.fdtprc.
    rc = r.base / 'awake.fdtprc'
    rc.write_text('AWAKE_MINUTES=0\n')
    s = r.run('awake0', 'M-i SNAP', d, ['--size', '80x25'], env={'FDTP_SYSINFO_ROOT': str(root), 'FDTP_RC_USER': str(rc)}, expect=1)[0]
    require(rows_of(s)['AWAKE_MINUTES'][0] == '0（使わない）', f'awake 0: {rows_of(s)["AWAKE_MINUTES"]}')
    print(f'sysinfo checks passed: {r.count} snapshots, {check_wide.checks} checks')


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
