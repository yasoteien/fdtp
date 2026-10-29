#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Select B in FDTP, download real bytes on loopback, see the record, Esc.

The tiny exec wrapper selects a free test port and records the real helper's
PID. All HTTP and multicast remain on loopback; no fake events are emitted.
"""
import argparse
import http.client
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
from urllib.parse import urlencode

from check import check_snapshot, require, text

ROOT = Path(__file__).resolve().parents[1]


def screen(snap):
    return '\n'.join(text(snap, y).rstrip() for y in range(len(snap['rows'])))


def run(build):
    base = build / 'localsend-browser-e2e'
    shutil.rmtree(base, ignore_errors=True)
    (base / 'files').mkdir(parents=True)
    (base / 'home').mkdir()
    source = base / 'files' / '日本語.txt'
    content = 'FDTP の画面から本物のブラウザ受け取りへ\n'.encode()
    source.write_bytes(content)
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    helper = build / 'fdtp-localsend'
    executable = build / ('fdtp-preview' if sys.platform == 'darwin' else 'fdtp')
    wrapper = base / 'helper.py'
    pid_file = base / 'helper.pid'
    wrapper.write_text(f'#!{sys.executable}\nimport os,sys\n'
                       f'open({str(pid_file)!r},"w").write(str(os.getpid()))\n'
                       f'os.execv({str(helper)!r}, [{str(helper)!r},sys.argv[1],'
                       f'"--port",{str(port)!r},*sys.argv[2:]])\n')
    wrapper.chmod(0o700)
    script = base / 'browser.keys'
    script.write_text('DOWN M-l b REALWAIT:1 SNAP\n'
                      'REALWAIT:5 END SNAP\nESC REALWAIT:0.2 SNAP\n')
    prefix = base / 'browser'
    env = dict(os.environ, HOME=str(base / 'home'), FDTP_LOCALSEND_LOOPBACK='1',
               FDTP_LOCALSEND_HELPER=str(wrapper), FDTP_LANGUAGE='ja', FDTP_HISTORY='',
               FDTP_RC_SYSTEM='', FDTP_RC_USER='', FDTP_POMERA_CHECK='', FDTP_POMERA_SEEN='')
    process = subprocess.Popen([str(executable), '--headless', '--script', str(script),
                                '--out', str(prefix), '--time', '1700000000', str(source.parent)],
                               env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    finished = False
    try:
        first = prefix.with_name(prefix.name + '-001.cells')
        deadline = time.monotonic() + 5
        while True:
            try:
                snap = check_snapshot(first)
                break
            except (FileNotFoundError, json.JSONDecodeError):
                require(time.monotonic() < deadline, 'FDTP did not produce browser-ready snapshot')
                time.sleep(0.02)
        shown = screen(snap)
        require(f'http://127.0.0.1:{port}' in shown, f'real URL not displayed: {shown}')
        match = re.search(r'PIN: (\d{6})', shown)
        require(match is not None, f'real helper PIN not displayed: {shown}')
        def request(method, path):
            conn = http.client.HTTPConnection('127.0.0.1', port, timeout=3)
            try:
                conn.request(method, path)
                response = conn.getresponse()
                return response.status, response.read()
            finally:
                conn.close()
        status, body = request('POST', '/api/localsend/v2/prepare-download?' + urlencode({'pin': match[1]}))
        require(status == 200, f'PIN shown by FDTP did not authenticate: {status}')
        session = json.loads(body)
        require(len(session['files']) == 1, 'cursor selection differs from real exported file set')
        file_id, meta = next(iter(session['files'].items()))
        require(meta['fileName'] == source.name, f'wrong exported source: {meta}')
        status, body = request('GET', '/api/localsend/v2/download?' +
                               urlencode({'sessionId': session['sessionId'], 'fileId': file_id}))
        require(status == 200 and body == content, 'real download content differs from selected file')
        _, errors = process.communicate(timeout=10)
        require(process.returncode == 0, f'FDTP browser E2E failed: {errors}')
        received = screen(check_snapshot(prefix.with_name(prefix.name + '-002.cells')))
        require('127.0.0.1が日本語.txtを受け取りました' in re.sub(r'\s+', '', received),
                f'real download completion not shown: {received}')
        closed = check_snapshot(prefix.with_name(prefix.name + '-003.cells'))
        require(closed['depth'] == 1, 'Esc did not return to filer')
        pid = int(pid_file.read_text())
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError('real helper survived Esc')
        finished = True
        print('LocalSend browser E2E passed: selection → PIN/URL → real download → record → Esc (loopback)')
    finally:
        if process.poll() is None:
            process.terminate()
            process.communicate(timeout=5)
        if not finished and pid_file.exists():
            try:
                os.kill(int(pid_file.read_text()), signal.SIGTERM)
            except ProcessLookupError:
                pass


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build')
    run(parser.parse_args().build.resolve())
