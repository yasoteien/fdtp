#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A3/A4 LocalSend screen acceptance checks with a network-free fake helper.

Run python3 -B tests/check_localsend_ui.py [--build BUILD]. All generated
fixtures, helper logs and snapshots remain in BUILD/localsend-ui-check.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys

from check import notice, popup_rows, require, screen_path, text
from check_popup import Runner

ROOT = Path(__file__).resolve().parents[1]
ALIAS = '試験 alias ;$(not-a-command)'
SPECIAL = 'b space \'";$().txt'
WARNING = ('同じネットワークにいる人には、この機器の名前が見えます。自宅など信頼できる LAN か、'
           'スマートフォンのインターネット共有（テザリング）で使ってください')
PLAINTEXT = '暗号化されません。自宅など信頼できる LAN だけで使ってください'


def body(snap):
    return '\n'.join(text(snap, y, end=len(snap['rows'][y])).rstrip() for y in range(len(snap['rows'])))


def compact(value):
    return re.sub(r'\s+', '', value)


def warning(snap):
    # Match the complete user-approved text even when wrapped across rows.
    require(compact(WARNING) in compact(body(snap)), f'complete LAN warning missing: {body(snap)}')


def args_after_options(argv):
    remaining = list(argv[1:])
    for flag in ('--to', '--alias', '--pin'):
        if flag in remaining:
            i = remaining.index(flag)
            del remaining[i:i + 2]
    if remaining and remaining[0] == '--':
        del remaining[0]
    return remaining


def flag(argv, name):
    require(name in argv and argv.index(name) + 1 < len(argv), f'{name} missing from {argv}')
    return argv[argv.index(name) + 1]


def children_finished(starts):
    for event in starts:
        try:
            os.kill(event['pid'], 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError(f'helper process survived the headless app: {event}')


class Suite:
    def __init__(self, build):
        self.base = build / 'localsend-ui-check'
        shutil.rmtree(self.base, ignore_errors=True)
        self.base.mkdir(parents=True)
        self.files = self.base / 'files'
        self.files.mkdir()
        (self.files / 'a folder').mkdir()
        (self.files / SPECIAL).write_text('special\n')
        (self.files / 'c.txt').write_text('ordinary\n')
        (self.base / 'home').mkdir()
        (build / 'tmp').mkdir(exist_ok=True)
        self.rc = self.base / 'fdtprc'
        self.rc.write_text(f"LOCALSEND_ALIAS='{ALIAS}'\n")
        self.env = dict(os.environ, HOME=str(self.base / 'home'), TMPDIR=str(build / 'tmp'), TZ='UTC',
                        FDTP_LANGUAGE='ja', FDTP_HISTORY='', FDTP_RC_SYSTEM='', FDTP_RC_USER=str(self.rc),
                        FDTP_POMERA_CHECK='', FDTP_POMERA_SEEN='',
                        FDTP_LOCALSEND_HELPER=str(ROOT / 'tests/localsend_fake.py'))
        self.runner = Runner(build, self.base)

    def run(self, name, mode='send', language='ja', missing=False, directory=None, extra=None):
        prefix = self.base / (name + '-' + language)
        log = prefix.with_suffix('.jsonl')
        log.unlink(missing_ok=True)
        env = dict(self.env, FDTP_LANGUAGE=language, FDTP_LOCALSEND_TEST_MODE=mode,
                   FDTP_LOCALSEND_TEST_LOG=str(log))
        if missing:
            env['FDTP_LOCALSEND_HELPER'] = str(self.base / 'no-such-helper')
        env.update(extra or {})
        snaps = self.runner.run(f'localsend_{name}.keys', directory or self.files, env, prefix)
        events = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        starts = [e for e in events if e['event'] == 'start']
        for event in starts:
            require(flag(event['argv'], '--alias') == ALIAS, f'alias not passed literally: {event}')
        # Every helper child must have finished by app exit; a live PID also
        # catches a missing SIGTERM on screen close rather than trusting logs.
        children_finished(starts)
        return snaps, events, [e['argv'] for e in starts]


def check_menu(suite):
    s, _events, commands = suite.run('menu')
    require(s['menu']['widget'] == 4, 'Alt+l opens the standard choice box')
    rows = ' '.join(value for _, value in popup_rows(s['menu']))
    require('LocalSend' in rows and '(S)送る' in rows and '(R)受け取る' in rows and '(B)ブラウザで渡す' in rows, rows)
    require(s['closed']['rows'] == s['initial']['rows'] and not commands, 'Esc restores filer without spawning helper')


def check_send(suite):
    s, events, commands = suite.run('send')
    warning(s['peers'])
    peers = body(s['peers'])
    require('台所 Mac updated' in peers and '携帯 phone' in peers and '192.0.2.10' in peers and '192.0.2.20' in peers, peers)
    require(peers.count('192.0.2.10') == 1, 'repeat fingerprint updates a peer instead of duplicating it')
    require(s['peers']['depth'] > 1 and s['peers']['widget'] == 0, 'S starts peer screen directly')
    for label in ('pin', 'invalid_pin', 'wrong_pin'):
        require(s[label]['widget'] == 3 and '相手の画面の PIN' in notice(s[label]), f'{label}: {notice(s[label])}')
    scans = [a for a in commands if a[0] == 'scan']
    sends = [a for a in commands if a[0] == 'send']
    require(len(scans) == 1 and flag(scans[0], '--seconds') == '0', f'continuous discovery: {scans}')
    require(len(sends) == 3, f'initial send plus two PIN retries: {sends}')
    require('--pin' not in sends[0] and flag(sends[1], '--pin') == '111111' and flag(sends[2], '--pin') == 'FxDR7m', sends)
    for argv in sends:
        require(flag(argv, '--to') == 'fingerprint-two', f'UP/DOWN selects peer by fingerprint: {argv}')
        require(args_after_options(argv) == [str(suite.files / SPECIAL), str(suite.files / 'c.txt')], f'all marked paths passed literally: {argv}')
    require('送りました' in body(s['sent']) and '2' in body(s['sent']), body(s['sent']))
    require(s['closed']['depth'] == 1, 'Esc returns from sending')
    scan_pids = {e['pid'] for e in events if e['event'] == 'start' and e['argv'][0] == 'scan'}
    require(scan_pids <= {e['pid'] for e in events if e['event'] == 'term'}, 'scan child stopped when sending begins')


def check_cursor(suite):
    s, _events, commands = suite.run('cursor', mode='send_ok')
    sends = [a for a in commands if a[0] == 'send']
    require(len(sends) == 1 and args_after_options(sends[0]) == [str(suite.files / 'a folder')], f'unmarked cursor directory: {sends}')
    require('送りました' in body(s['sent']), body(s['sent']))
    s, _events, commands = suite.run('parent')
    require(not commands and s['parent']['depth'] == 1, '.. cannot be sent')
    require('選んで' in notice(s['parent']), notice(s['parent']))


def check_receive(suite):
    s, events, commands = suite.run('receive', mode='receive')
    for label in ('ready', 'ready_again', 'saved', 'rejected'):
        warning(s[label])
    require(s['ip_back']['rows'] == s['ready']['rows'], 'PgUp restores the first IP page')
    if s['ip_page']['rows'] != s['ready']['rows']:
        require(s['ip_page']['widget'] == 0, 'PgDn scrolls IPs without opening a popup')
    ready = body(s['ready'])
    require(ALIAS in ready, f'own alias shown: {ready}')
    require(compact(screen_path(suite.files)) in compact(ready), f'receive destination shown: {ready}')
    require('192.0.2.99' in ready or re.search(r'\b(?:\d{1,3}\.){3}\d{1,3}\b', ready), f'IP shown: {ready}')
    receivers = [a for a in commands if a[0] == 'receive']
    require(len(receivers) == 2, f'opens receiving twice: {receivers}')
    pins = []
    for argv in receivers:
        require('--ask' in argv and flag(argv, '--dir') == str(suite.files), f'ask and absolute destination: {argv}')
        pin = flag(argv, '--pin')
        require(re.fullmatch(r'\d{6}', pin), f'six digit PIN: {pin}')
        pins.append(pin)
    require(pins[0] != pins[1], 'fresh PIN each time receiving opens')
    for label, pin in [('ready', pins[0]), ('ready_again', pins[1])]:
        require(pin in compact(body(s[label])), f'{label}: displayed PIN matches helper')
    for label, name in [('request', '送り主 sender'), ('request_again', '次の相手 second'), ('default_request', '既定の相手 default')]:
        require(s[label]['widget'] == 2 and name in notice(s[label]), f'{label}: sender confirmation: {notice(s[label])}')
    require([e['answer'] for e in events if e['event'] == 'answer'] == ['accept', 'reject', 'reject'], f'y/n to helper stdin: {events}')
    # The photo of the receive screen (revision 1 §10): no line begins with
    # 、 or 。, and only IPv4 addresses are told (no loopback, fe80 or IPv6).
    shown = body(s['ready'])
    require(not any(line.strip().startswith(('、', '。')) for line in shown.splitlines()), f'kinsoku: {shown}')
    require(not re.search(r'\blo0?:|127\.0\.0\.1|::1|fe80:', shown) and re.search(r'\d+\.\d+\.\d+\.\d+', shown),
            f'only IPv4 addresses: {shown}')
    require(s['saved']['depth'] > 1 and s['saved']['widget'] == 0 and 'received 日本語.txt' in body(s['saved']), 'saved shown and done keeps receiving')
    require('1個を受け取りました' in compact(body(s['saved'])), f'done says how many: {body(s["saved"])}')
    require(s['rejected']['depth'] > 1 and s['rejected_default']['depth'] > 1, 'n and default Enter reject keep receiving')
    require(s['closed']['depth'] == 1 and 'received' in body(s['closed']), 'Esc reloads filer to show saved file')
    receiver_pids = {e['pid'] for e in events if e['event'] == 'start'}
    require(receiver_pids <= {e['pid'] for e in events if e['event'] == 'term'}, 'Esc SIGTERMs each receiver')


def check_gone(suite):
    s, events, _commands = suite.run('gone', mode='gone')
    require(s['asked']['widget'] == 2 and '去る人' in notice(s['asked']), f'asked: {notice(s["asked"])}')
    require(s['gone']['widget'] == 0 and '相手が取りやめました' in compact(body(s['gone'])), f'the question closes: {body(s["gone"])}')
    require('届かないので取りやめました' in compact(body(s['idle'])), f'idle sender: {body(s["idle"])}')
    require(not [e for e in events if e['event'] == 'answer'], 'no answer is sent for a sender that left')


def check_official(suite):
    """FDTM on a Mac (docs/FDTP_MACOS.md): beside the official LocalSend app.
    A file stands for it (FDTP_LOCALSEND_OFFICIAL_FILE); the fake helper
    makes it in official_later, as if the app started while FDTM accepts."""
    official = suite.base / 'official-running'

    def run(name, mode, setting=None, running=False):
        official.unlink(missing_ok=True)
        if running:
            official.touch()
        extra = {'FDTP_LOCALSEND_OFFICIAL_FILE': str(official)}
        if setting:
            rc = suite.base / f'fdtprc-{setting}'
            rc.write_text(f"LOCALSEND_ALIAS='{ALIAS}'\nLOCALSEND_OFFICIAL={setting}\n")
            extra['FDTP_RC_USER'] = str(rc)
        return suite.run(name, mode=mode, extra=extra)

    question = '公式の LocalSend が起動しました。どちらで受け取りますか'
    left = '公式の LocalSend アプリに任せました'
    s, events, commands = run('official_asked', 'official_later')
    require(s['asked']['widget'] == 4 and question in notice(s['asked']), f'asked: {notice(s["asked"])}')
    rows = ' '.join(value for _, value in popup_rows(s['asked']))
    require('(F)FDTM で続ける' in rows and '(L)公式アプリを使う' in rows, rows)
    require(compact(left) in compact(body(s['left'])) and s['left']['depth'] > 1, f'left: {body(s["left"])}')
    pids = {e['pid'] for e in events if e['event'] == 'start'}
    require(len(commands) == 1 and pids <= {e['pid'] for e in events if e['event'] == 'term'}, 'the receiver is stopped for the app')
    require(s['closed']['depth'] == 1, 'Esc goes back to the filer')

    s, events, commands = run('official_keep', 'official_later')
    require(s['asked']['widget'] == 4, 'asked again in a new run')
    require(s['kept']['widget'] == 0 and 'FDTMで受け付けを続けます' in compact(body(s['kept'])), body(s['kept']))
    require(s['not_again']['widget'] == 0 and compact('FDTM で受け付けを続けます') in compact(body(s['not_again'])),
            'kept: no second question while the app stays')
    require(len(commands) == 1, f'one receiver all along: {commands}')
    require(s['closed']['depth'] == 1, 'Esc closes')

    s, _events, commands = run('official_browser', 'official_later')
    require([a[0] for a in commands] == ['serve'] and question in notice(s['asked']), f'browser asked: {notice(s["asked"])}')
    require(compact(left) in compact(body(s['left'])), body(s['left']))

    s, _events, _commands = run('official_setting', 'official_later', setting='official')
    require(s['decided']['widget'] == 0 and compact(left) in compact(body(s['decided'])), f'always official: {body(s["decided"])}')
    s, _events, _commands = run('official_setting', 'official_later', setting='fdtm')
    require(s['decided']['widget'] == 0 and '受け取りを待っています' in compact(body(s['decided'])), f'always FDTM: {body(s["decided"])}')

    refused = '公式の LocalSend アプリが動いています'
    s, _events, commands = run('official_running', 'send', running=True)
    for label in ('receive_refused', 'browser_refused'):
        require(compact(refused) in compact(notice(s[label])) and s[label]['depth'] == 1, f'{label}: {notice(s[label])}')
    require([a[0] for a in commands] == ['scan'] and s['sending']['depth'] > 1, f'sending still works: {commands}')
    s, _events, commands = run('official_running', 'send', setting='fdtm', running=True)
    require([a[0] for a in commands][:2] == ['receive', 'serve'], f'LOCALSEND_OFFICIAL=fdtm receives anyway: {commands}')
    official.unlink(missing_ok=True)


def browser_pin(snap, pin):
    rows = [y for y in range(len(snap['rows'])) if f'PIN: {pin}' in text(snap, y, end=len(snap['rows'][y]))]
    require(len(rows) == 1, f'displayed browser PIN matches helper: {body(snap)}')
    digits = ('111101101101111', '010110010010111', '111001111100111', '111001111001111', '101101111001001',
              '111100111001111', '111100111101111', '111001001001001', '111101111101111', '111101111001111')
    left = (len(snap['rows'][0]) - 29) // 2
    for i, digit in enumerate(pin):
        for y in range(5):
            for x in range(3):
                expected = 3 if digits[int(digit)][y * 3 + x] == '1' else 0
                require(snap['rows'][rows[0] + 1 + y][left + i * 5 + x][2] == expected,
                        f'large browser PIN digit {i}, row {y}, col {x}')


def check_browser(suite):
    s, events, commands = suite.run('browser', mode='browser')
    require(len(commands) == 2 and all(a[0] == 'serve' for a in commands), f'B starts only serve: {commands}')
    pins = []
    for argv in commands:
        pin = flag(argv, '--pin')
        require(re.fullmatch(r'\d{6}', pin), f'six-digit browser PIN: {argv}')
        pins.append(pin)
        require(argv.index('--') == argv.index('--pin') + 2, f'option terminator before files: {argv}')
        require(args_after_options(argv) == [str(suite.files / SPECIAL), str(suite.files / 'c.txt')],
                f'marked browser paths passed literally: {argv}')
    require(pins[0] != pins[1], 'browser PIN refreshed on reopening')
    for label in ('ready', 'wrong', 'downloading', 'downloaded', 'home', 'ready_again'):
        warning(s[label])
        value = compact(body(s[label]))
        require(compact(PLAINTEXT) in value, f'{label}: plaintext warning stays visible')
        require(s[label]['widget'] == 0 and s[label]['depth'] == 2, f'{label}: browser screen has no dialog')
        browser_pin(s[label], pins[1] if label == 'ready_again' else pins[0])
    ready = body(s['ready'])
    require('http://192.0.2.99:53317' in ready and 'http://198.51.100.99:53317' in ready, f'all URLs: {ready}')
    require('ブラウザで次の所' in ready and SPECIAL in ready and 'c.txt' in ready, f'instructions and selected files: {ready}')
    require('192.0.2.5がPINを間違えました' in compact(body(s['wrong'])), body(s['wrong']))
    require('192.0.2.5に日本語.txtを渡しています' in compact(body(s['downloading'])), body(s['downloading']))
    downloaded = compact(body(s['downloaded']))
    require('192.0.2.5が日本語.txtを受け取りました' in downloaded, body(s['downloaded']))
    require('198.51.100.5がsecond.txtを受け取りました' in downloaded, body(s['downloaded']))
    require('http://192.0.2.99:53317' in body(s['home']), 'Home returns to URLs')
    require(s['closed']['depth'] == s['closed_again']['depth'] == 1, 'Esc closes browser screens')
    require({e['pid'] for e in events if e['event'] == 'start'} <= {e['pid'] for e in events if e['event'] == 'term'},
            'Esc SIGTERMs every serving child')
    s, _events, commands = suite.run('browser_cursor', mode='browser')
    require(len(commands) == 1 and args_after_options(commands[0]) == [str(suite.files / 'a folder')],
            f'cursor directory passed for browser sharing: {commands}')
    s, _events, commands = suite.run('browser_parent')
    require(not commands and s['parent']['depth'] == 1 and '選んで' in notice(s['parent']), '.. cannot be shared')
    s, _events, commands = suite.run('browser_busy', mode='browser_busy')
    require('他のLocalSend' in compact(body(s['busy'])) and '53317' in body(s['busy']) and '送る' in body(s['busy']), body(s['busy']))
    require(len(commands) == 1 and commands[0][0] == 'serve', 'port_busy does not try another mode')
    for mode, message in (('browser_error', 'ファイルを読み書きできません'), ('browser_pin_limit', 'PINが違います')):
        s, _events, _commands = suite.run('browser_error', mode=mode)
        require(message in compact(body(s['error'])), f'{mode}: {body(s["error"])}')
    s, _events, commands = suite.run('browser_cursor', mode='browser', missing=True)
    require('fdtp-localsend' in body(s['ready']) and 'ありません' in body(s['ready']) and not commands, 'missing browser helper explained')
    s, _events, commands = suite.run('browser', mode='browser', language='en')
    require('Share with a browser' in body(s['ready']) and 'not encrypted' in body(s['ready']), body(s['ready']))
    require('Open one of these addresses' in body(s['ready']) and 'Items to share:' in body(s['ready']), 'English browser instructions')
    require('incorrect PIN' in body(s['wrong']) and 'sending' in body(s['downloading']), 'English browser status')
    require('192.0.2.5 downloaded' in body(s['downloaded']) and '受け取りました' not in body(s['downloaded']), 'English browser completion')


def check_browser_many(suite):
    directory = suite.base / 'many-browser-files'
    directory.mkdir()
    names = [f'item-{i:02}.txt' for i in range(35)] + ['very-long-name-' * 9 + '.txt', '最後の日本語.txt']
    for name in names:
        (directory / name).write_text(name)
    s, events, commands = suite.run('browser_many', mode='browser_many', directory=directory)
    require(len(commands) == 1 and set(args_after_options(commands[0])) == {str(directory / name) for name in names},
            f'all marked items reach serve: {commands}')
    require(s['top']['rows'] == s['back']['rows'] == s['up']['rows'] == s['home']['rows'], 'PgUp/Up/Home return to first browser page')
    require(s['page']['rows'] != s['top']['rows'] and s['down']['rows'] != s['top']['rows'], 'PgDn/Down scroll browser contents')
    pages = [snap for label, snap in s.items() if label != 'closed']
    ordered_pages = [s['top']] + [s[f'page{i:02}'] for i in range(1, 25)]
    def scroll_text(snap):
        start = next(y for y in range(len(snap['rows'])) if 'ブラウザからの受け取りを待っています' in text(snap, y)) + 1
        return ''.join(text(snap, y).rstrip() for y in range(start, len(snap['rows']) - 1))
    all_text = compact(''.join(scroll_text(snap) for snap in ordered_pages))
    for url in ['http://192.0.2.99:53317', 'http://198.51.100.99:53317'] + [f'http://203.0.113.{i}:53317' for i in range(1, 25)]:
        require(url in all_text, f'URL reachable by paging: {url}')
    require('long-address/' * 12 in all_text, 'long URL wraps without truncation')
    for name in names:
        require(name in all_text, f'selected file reachable by paging: {name}')
    for i in range(6, 70):
        require(f'completed-{i:02}.txt' in all_text, f'completed download retained and reachable: {i}')
    require('completed-00.txt' not in all_text and 'completed-05.txt' not in all_text, 'history retains only the latest 64 records')
    require('completed-69.txt' in body(s['end']), 'End reaches latest completed download')
    for snap in pages:
        warning(snap)
        require(compact(PLAINTEXT) in compact(body(snap)), 'both warnings stay on every page')
        browser_pin(snap, flag(commands[0], '--pin'))
    require(s['closed']['depth'] == 1, 'large browser screen closes')


def check_errors_and_english(suite):
    s, _events, commands = suite.run('busy', mode='busy')
    value = body(s['busy'])
    require('他の LocalSend' in value and '53317' in value and '送る' in value, f'port_busy explanation: {value}')
    require(len(commands) == 1 and commands[0][0] == 'receive', 'busy only attempts receiving')
    s, _events, commands = suite.run('missing', missing=True)
    require('fdtp-localsend' in body(s['missing']) and 'ありません' in body(s['missing']) and not commands, body(s['missing']))
    s, _events, _commands = suite.run('menu', language='en')
    value = ' '.join(value for _, value in popup_rows(s['menu']))
    require('LocalSend' in value and 'Send' in value and 'Receive' in value and 'Share with a browser' in value and '送る' not in value, f'English menu: {value}')
    s, _events, _commands = suite.run('cursor', mode='send_ok', language='en')
    value = body(s['peers'])
    require('network' in value.lower() and 'テザリング' not in value, f'English warning: {value}')
    require('送りました' not in body(s['sent']), 'English completed-send message')



def check_alias_customizer(suite):
    s, _events, commands = suite.run('alias')
    require('LOCALSEND_ALIAS' in body(s['alias']) and ALIAS in body(s['alias']), 'customizer shows alias setting')
    require('edited alias' in body(s['edited']) and 'edited alias' in body(s['reopened']), 'alias editable and retained after reopening')
    require("LOCALSEND_ALIAS=" in suite.rc.read_text() and 'edited alias' in suite.rc.read_text(), 'alias written to user configuration')
    require(not commands, 'customizing alias does not start a helper')
    # Other cases use the same deliberate shell metacharacters as before.
    suite.rc.write_text(f"LOCALSEND_ALIAS='{ALIAS}'\n")



def check_wide(suite):
    prefix = suite.base / 'wide'
    log = suite.base / 'wide.jsonl'
    env = dict(suite.env, FDTP_LOCALSEND_TEST_MODE='send_ok', FDTP_LOCALSEND_TEST_LOG=str(log))
    subprocess.run([str(suite.runner.executable), '--headless', '--size', '120x36', '--script',
                    str(ROOT / 'tests/localsend_wide.keys'), '--out', str(prefix), '--time', '1700000000',
                    str(suite.files)], check=True, cwd=ROOT.parent, env=env, stdout=subprocess.DEVNULL)
    events = [json.loads(line) for line in log.read_text().splitlines()]
    children_finished([e for e in events if e['event'] == 'start'])
    snaps = [json.loads(p.read_text()) for p in sorted(suite.base.glob('wide-*.cells'))]
    require(len(snaps) == 3, 'wide peer, receive and browser snapshots')
    for snap in snaps:
        require(len(snap['rows']) == 36 and all(len(row) == 120 for row in snap['rows']), '120x36 screen size')
        warning(snap)
        require(snap['depth'] == 2 and snap['widget'] == 0, 'wide screen active without a popup')
    require('192.0.2.20' in body(snaps[0]) and '受け取り先' in body(snaps[1]), 'wide peers and receive destination')
    require(compact(PLAINTEXT) in compact(body(snaps[2])) and 'http://198.51.100.99:53317' in body(snaps[2]), 'wide browser warning and URLs')
    browser_pin(snaps[2], flag(next(e['argv'] for e in events if e['event'] == 'start' and e['argv'][0] == 'serve'), '--pin'))
    suite.runner.snapshots += len(snaps)
    print('  localsend_wide.keys: 3 snapshots (120x36)')



def check_helper_discovery(suite):
    # An isolated executable copy prevents the built real helper being found.
    # PATH contains only our test directory and a python3 symlink for the fake's
    # /usr/bin/env shebang, so a lookup bug cannot contact a real network helper.
    executable_dir = suite.base / 'discovery-executable'
    path_dir = suite.base / 'discovery-path'
    executable_dir.mkdir()
    path_dir.mkdir()
    copied_executable = executable_dir / suite.runner.executable.name
    shutil.copy2(suite.runner.executable, copied_executable)
    helper = executable_dir / 'fdtp-localsend'
    shutil.copy2(ROOT / 'tests/localsend_fake.py', helper)
    (path_dir / 'python3').symlink_to(Path(sys.executable).resolve())
    original_executable = suite.runner.executable
    suite.runner.executable = copied_executable
    try:
        for where in ('sibling', 'path'):
            if where == 'path':
                moved_helper = path_dir / helper.name
                helper.rename(moved_helper)
                helper = moved_helper
            prefix = suite.base / ('discovery-' + where)
            log = prefix.with_suffix('.jsonl')
            env = dict(suite.env, PATH=str(path_dir), FDTP_LOCALSEND_TEST_MODE='send_ok',
                       FDTP_LOCALSEND_TEST_LOG=str(log))
            env.pop('FDTP_LOCALSEND_HELPER', None)
            if where == 'path':
                env['FDTP_RC_USER'] = ''
            s = suite.runner.run('localsend_cursor.keys', suite.files, env, prefix)
            events = [json.loads(line) for line in log.read_text().splitlines()]
            starts = [e for e in events if e['event'] == 'start']
            require(len(starts) == 2 and [e['argv'][0] for e in starts] == ['scan', 'send'], f'{where}: helper scan and send: {starts}')
            expected_alias = ALIAS if where == 'sibling' else socket.gethostname()[:255]
            for event in starts:
                require(event['executable'] == str(helper), f'{where}: executed only fixture helper: {event}')
                require(flag(event['argv'], '--alias') == expected_alias, f'{where}: configured alias or hostname fallback: {event}')
            require(args_after_options(starts[1]['argv']) == [str(suite.files / 'a folder')], f'{where}: cursor path reaches discovered helper')
            require('送りました' in body(s['sent']), f'{where}: discovered helper completes send')
            children_finished(starts)
    finally:
        suite.runner.executable = original_executable


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build')
    suite = Suite(parser.parse_args().build.resolve())
    check_menu(suite)
    check_alias_customizer(suite)
    check_send(suite)
    check_cursor(suite)
    check_receive(suite)
    check_gone(suite)
    check_official(suite)
    check_browser(suite)
    check_browser_many(suite)
    check_errors_and_english(suite)
    check_wide(suite)
    check_helper_discovery(suite)
    print(f'LocalSend UI checks passed: {suite.runner.snapshots} snapshots; no network helper used')


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError, subprocess.CalledProcessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
