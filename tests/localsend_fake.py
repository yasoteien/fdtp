#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Network-free fdtp-localsend stand-in used by check_localsend_ui.py.

Only the fixture's files, JSONL invocation log and stdout/stdin are touched.
The UI must pass arguments directly: names deliberately contain shell syntax.
"""
import json
import os
from pathlib import Path
import signal
import sys
import time

LOG = Path(os.environ['FDTP_LOCALSEND_TEST_LOG'])
MODE = os.environ.get('FDTP_LOCALSEND_TEST_MODE', 'send')
ARGV = sys.argv[1:]


def record(event, **values):
    line = json.dumps(dict(event=event, pid=os.getpid(), **values), ensure_ascii=False) + '\n'
    fd = os.open(LOG, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600)
    try:
        os.write(fd, line.encode())
    finally:
        os.close(fd)


def terminate(signum, _frame):
    record('term', signal=signum)
    raise SystemExit(0)


def emit(value):
    sys.stdout.write(value)
    sys.stdout.flush()


def option(name, default=None):
    return ARGV[ARGV.index(name) + 1] if name in ARGV else default


def linger():
    while True:
        time.sleep(1)


def official_starts():
    """FDTM on a Mac (docs/FDTP_MACOS.md): the official app starts meanwhile."""
    time.sleep(0.2)
    Path(os.environ['FDTP_LOCALSEND_OFFICIAL_FILE']).touch()
    linger()


def main():
    signal.signal(signal.SIGTERM, terminate)
    record('start', argv=ARGV, executable=str(Path(sys.argv[0]).resolve()))
    if ARGV[0] == 'scan':
        # The first record crosses read boundaries; the rest arrive together.
        emit('peer\tfingerprint-one\t台所 ')
        time.sleep(0.04)
        emit('Mac\t192.0.2.10\t53317\thttps\tdesktop\tMacBook\n'
             'peer\tfingerprint-two\t携帯 phone\t192.0.2.20\t53318\thttps\tmobile\tiPhone\n'
             'peer\tfingerprint-one\t台所 Mac updated\t192.0.2.10\t53317\thttps\tdesktop\tMacBook\n')
        linger()
    elif ARGV[0] == 'send':
        pin = option('--pin')
        if MODE != 'send_ok' and pin is None:
            emit('pin\trequired\nerror\tpin\tpeer PIN is required\n')
            return 1
        elif MODE != 'send_ok' and pin != 'FxDR7m':
            emit('pin\twrong\nerror\tpin\tincorrect PIN\n')
            return 1
        else:
            emit('file\tb special.txt\t10\nprogress\tb special.txt\t10\t10\ndone\n')
    elif ARGV[0] == 'receive':
        if MODE == 'busy':
            emit('error\tport_busy\tfixture busy\n')
            return 1
        emit('ready\t53317\thttps://192.0.2.99:53317\n')
        if MODE == 'official_later':
            official_starts()
        if MODE == 'gone':
            # The sender quits while asked, then one is given up for silence.
            time.sleep(0.9)
            emit('request\t去る人 leaver\t1\t12\n')
            time.sleep(0.9)
            emit('cancelled\tpeer\n')
            time.sleep(0.6)
            emit('cancelled\ttimeout\n')
            linger()
            return 0
        time.sleep(0.9)
        emit('request\t送り主 sender\t2\t2097152\n')
        answer = sys.stdin.readline().rstrip('\r\n')
        record('answer', answer=answer)
        if answer == 'accept':
            saved = Path(option('--dir')) / 'received 日本語.txt'
            saved.write_text('received by fake helper\n')
            emit('file\treceived 日本語.txt\t24\nprogress\treceived 日本語.txt\t24\t24\n'
                 f'saved\t{saved}\ndone\n')
        time.sleep(0.9)
        emit('request\t次の相手 second\t1\t12\n')
        answer = sys.stdin.readline().rstrip('\r\n')
        record('answer', answer=answer)
        time.sleep(0.6)
        emit('request\t既定の相手 default\t1\t12\n')
        answer = sys.stdin.readline().rstrip('\r\n')
        record('answer', answer=answer)
        linger()
    elif ARGV[0] == 'serve':
        assert '--' in ARGV and ARGV.index('--') > ARGV.index('--pin')
        if MODE == 'browser_busy':
            emit('error\tport_busy\tfixture busy\n')
            return 1
        emit('ready\t53317\thttp://192.0.2.')
        time.sleep(0.04)
        emit('99:53317\nurl\thttp://198.51.100.99:53317\n')
        if MODE == 'official_later':
            official_starts()
        if MODE == 'browser_many':
            for i in range(1, 25):
                emit(f'url\thttp://203.0.113.{i}:53317\n')
            emit('url\thttp://203.0.113.250:53317/' + 'long-address/' * 12 + '\n')
            for i in range(70):
                emit(f'downloaded\t192.0.2.5\tcompleted-{i:02}.txt\n')
            linger()
        if MODE == 'browser_error':
            emit('error\tio\tfixture io error\n')
            return 1
        if MODE == 'browser_pin_limit':
            for _ in range(5):
                emit('pin\twrong\t192.0.2.5\n')
            emit('error\tpin\ttoo many incorrect PINs\n')
            return 1
        time.sleep(0.7)
        emit('pin\twrong\t192.0.2.5\n')
        time.sleep(0.6)
        emit('download\t192.0.2.5\t日本語.txt\n')
        time.sleep(0.6)
        emit('downloaded\t192.0.2.5\t日本語.txt\n'
             'downloaded\t198.51.100.5\tsecond.txt\n')
        linger()
    else:
        raise AssertionError(f'unexpected helper command: {ARGV}')


if __name__ == '__main__':
    try:
        raise SystemExit(main() or 0)
    finally:
        record('exit')
