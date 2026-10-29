#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A stand-in for fdtp-localsend for the manual's screen pictures
(tools/manual_shots.sh, FDTP_LOCALSEND_HELPER). No network: it prints what
the real helper prints, with example names and example addresses
(192.0.2.x, RFC 5737), so that no real address or name is photographed.
Answers to the receive question come on stdin as with the real helper.
"""
import signal
import sys
import time

ARGV = sys.argv[1:]


def emit(text):
    sys.stdout.write(text)
    sys.stdout.flush()


def option(name):
    return ARGV[ARGV.index(name) + 1] if name in ARGV else None


def linger():
    while True:
        time.sleep(1)


def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    command = ARGV[0]
    if command == 'scan':
        emit('peer\tA1B2C3D4E5F60718293A4B5C6D7E8F90A1B2C3D4E5F60718293A4B5C6D7E8F90\tiPhone\t192.0.2.20\t53317\thttps\tmobile\tiPhone\n'
             'peer\t0F1E2D3C4B5A69788796A5B4C3D2E1F00F1E2D3C4B5A69788796A5B4C3D2E1F0\tMacBook Air\t192.0.2.30\t53317\thttps\tdesktop\tmacOS\n')
        linger()
    elif command == 'send':
        if option('--pin') is None:
            emit('pin\trequired\nerror\tpin\tpeer PIN is required\n')
            return 1
        names = [a.rsplit('/', 1)[-1] for a in ARGV[ARGV.index('--to') + 2:] if not a.startswith('--')]
        for name in names:
            emit(f'file\t{name}\t2048\nprogress\t{name}\t2048\t2048\n')
        emit('done\n')
        linger()
    elif command == 'receive':
        emit('ready\t53317\thttps://192.0.2.36:53317\n')
        time.sleep(0.8)
        emit('request\tiPhone\t1\t2516582\n')
        if sys.stdin.readline().strip() == 'accept':
            folder = option('--dir')
            emit('file\tIMG_0001.JPG\t2516582\nprogress\tIMG_0001.JPG\t2516582\t2516582\n'
                 f'saved\t{folder}/IMG_0001.JPG\ndone\n')
        linger()
    elif command == 'serve':
        emit('ready\t53317\thttp://192.0.2.36:53317\n')
        time.sleep(0.6)
        first = ARGV[ARGV.index('--') + 1].rsplit('/', 1)[-1]
        emit(f'download\t192.0.2.20\t{first}\ndownloaded\t192.0.2.20\t{first}\n')
        linger()
    return 0


if __name__ == '__main__':
    sys.exit(main())
