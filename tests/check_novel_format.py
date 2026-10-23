#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Publisher format, Web/manuscript forms and the Word export of the novel
editor (docs/FDTP_REVISION1.md 4): check_novel_format.py --build DIR [--no-sanitize]

Builds like check_novel.py (DIR and DIR/novel-asan), links
tests/novel_format_tool.c, and checks:
  * rules 1-5 and 7 against the user's own scripts (syosetu_format.pl with
    perl, han2zen.py with python3), copied under DIR (the scripts rename and
    rewrite the files they are given): every line of copied episodes;
  * the original pipeline as it is (perl, then han2zen) against rules 1-5;
  * rules 6 and 8-12, the switches, the forms, undo, the Word export (zip
    entries, sectPr, paragraphs, w:ruby, w:em) and the F2 keys in the editor.
The user's files are only read. Everything is written under DIR/novel-format-check.

The episodes and scripts are the author's own and are not in the sources:
FDTP_NOVELS_DIR (or --novels) is the directory of works (each a directory of
episode_*.txt) and FDTP_NOVEL_WORK (or --work) the work whose script/ holds
syosetu_format.pl, han2zen.py and the Word template. Both can also be set in
the file of tests/dev_env.py. Without them the test is skipped (exit 0).
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_novel as cn  # noqa: E402
import dev_env  # noqa: E402

require, Failure = cn.require, cn.Failure
ROOT = cn.ROOT
TESTS = cn.TESTS
# Set in main() from --novels / --work (FDTP_NOVELS_DIR / FDTP_NOVEL_WORK).
NOVELS = WORK_NAME = SCRIPTS = None
RULES = ['ZENKAKU', 'LEADER', 'EXCLAIM', 'EXCLAIM_KEEP', 'SPACES', 'LEADER2', 'HALFSPACE', 'INDENT',
         'PERIOD', 'DASH', 'LEADER_EVEN', 'SCENE', 'BLANK']
W = '{http://schemas.openxmlformats.org/wordprocessingml/2006/main}'
WIDGET_NOTICE, WIDGET_CONFIRM, WIDGET_CHOICE, WIDGET_INFO, WIDGET_ERROR = 1, 2, 4, 5, 6


def rules(*on):
    return ''.join('1' if name in on else '0' for name in RULES)


ALL = rules(*RULES)
ORIGINAL = rules('ZENKAKU', 'LEADER', 'EXCLAIM', 'EXCLAIM_KEEP', 'SPACES')
ORIGINAL7 = rules('ZENKAKU', 'LEADER', 'EXCLAIM', 'EXCLAIM_KEEP', 'SPACES', 'HALFSPACE')


def han2zen(text):
    return text.translate({0x21 + i: 0xFF01 + i for i in range(94)})


# ---- build -----------------------------------------------------------------
def build(directory, sanitize):
    harness, fake, unit = cn.build(directory, sanitize)
    flags = ['-Werror'] + (['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitize else [])
    env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(directory / 'tmp'))
    ldflags = ['-fsanitize=address,undefined'] if sanitize else []
    make = ['make', '-s', '-C', str(ROOT), f'BUILD={directory}', 'CFLAGS=' + ' '.join(flags), 'LDFLAGS=' + ' '.join(ldflags)]
    info = subprocess.run(make + ['novel-link-info'], check=True, env=env, capture_output=True, text=True).stdout
    objects = libraries = None
    for line in info.splitlines():
        if line.startswith('OBJS '):
            objects = line[5:].split()
        elif line.startswith('LIBS '):
            libraries = shlex.split(line[5:])
    cc = shlex.split(os.environ.get('CC', 'cc'))
    tool = directory / 'novel-format-tool'
    subprocess.run(cc + ['-std=c11', '-Wall', '-Wextra'] + flags + ['-I' + str(ROOT / 'src'), '-I' + str(directory),
                         str(TESTS / 'novel_format_tool.c')] + objects + ['-o', str(tool)] + libraries, check=True, env=env)
    return harness, tool


class Tool:
    def __init__(self, path, work):
        self.path, self.work = path, work
        self.env = dict(os.environ, FDTP_LANGUAGE='ja', ASAN_OPTIONS='halt_on_error=1:detect_leaks=1' if sys.platform != 'darwin'
                        else 'halt_on_error=1:detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1',
                        TMPDIR=str(work / 'tmp'))
        self.n = 0

    def run(self, *args, expect=0):
        r = subprocess.run([str(self.path)] + [str(a) for a in args], capture_output=True, text=True, env=self.env,
                           timeout=600)
        require('runtime error' not in r.stderr and 'AddressSanitizer' not in r.stderr,
                f'tool {args[0]}: sanitizer\n{r.stderr[-3000:]}')
        require(r.returncode == expect, f'tool {args}: exit {r.returncode}\n{r.stderr[-2000:]}')
        return r.stdout

    def convert(self, mode, arg, text_or_path, crlf=False, encoding='utf-8'):
        """Returns (lines of the output, raw bytes, log lines, changed)."""
        self.n += 1
        d = self.work / 'tool'
        d.mkdir(parents=True, exist_ok=True)
        if isinstance(text_or_path, Path):
            src = text_or_path
        else:
            src = d / f'in{self.n}.txt'
            data = text_or_path.replace('\n', '\r\n') if crlf else text_or_path
            src.write_bytes(data.encode(encoding))
        out, log = d / f'out{self.n}.txt', d / f'log{self.n}.txt'
        args = [mode] + ([arg] if arg is not None else []) + [src, out, log]
        changed = int(self.run(*args).strip())
        raw = out.read_bytes()
        return raw.decode(encoding).replace('\r\n', '\n').split('\n'), raw, log.read_text(encoding='utf-8').splitlines(), changed

    def fmt(self, text, rule=ALL):
        lines, _, log, _ = self.convert('format', rule, text)
        return lines, log

    def pieces(self, rule, path):
        return [json.loads(line) for line in self.run('pieces', rule, path).splitlines()]


# ---- the user's scripts ------------------------------------------------------
class Original:
    """syosetu_format.pl and han2zen.py run on copies in their own directory."""

    def __init__(self, work):
        self.dir = work / 'perl'
        self.dir.mkdir(parents=True, exist_ok=True)
        for name in ('syosetu_format.pl', 'han2zen.py'):
            shutil.copyfile(SCRIPTS / name, self.dir / name)
        self.n = 0

    def perl(self, lines):
        """lines without line ends -> (lines, {perl line number: [messages]})."""
        self.n += 1
        name = f'p{self.n}.txt'
        (self.dir / name).write_bytes(''.join(line + '\n' for line in lines).encode('utf-8'))
        r = subprocess.run(['perl', 'syosetu_format.pl', f'-f={name}'], cwd=self.dir, capture_output=True, timeout=600)
        require(r.returncode == 0, f'perl: {r.stderr[-500:]}')
        out = (self.dir / name).read_bytes().decode('utf-8')
        require(out.endswith('\n') or not lines, 'perl output ends with a line end')
        messages = {}
        for line in r.stdout.decode('utf-8').splitlines():
            m = re.match(r'^(\d+)行目(.*)$', line)
            if m:
                messages.setdefault(int(m.group(1)), []).append(m.group(2))
        (self.dir / name).unlink()
        (self.dir / (name + '.old')).unlink()
        return out.split('\n')[:-1], messages

    def han2zen(self, data):
        r = subprocess.run(['python3', '-I', str(self.dir / 'han2zen.py')], input=data, capture_output=True, timeout=600,
                           env=dict(os.environ, FDTP_LANGUAGE='ja', PYTHONIOENCODING='utf-8', LC_ALL='en_US.UTF-8'))
        require(r.returncode == 0, f'han2zen: {r.stderr[-500:]}')
        return r.stdout


def expected_by_pieces(original, pieces, rule_halfspace):
    """Rules 1-5 (and 7) in FDTP's order with the user's scripts: han2zen,
    then (7) without spaces, then syosetu_format.pl on each piece outside the
    text in 《》. Returns (lines, {line: set of messages}, lines with more
    than one piece)."""
    feed, where = [], []
    for p in pieces:
        pre = p['pre']
        cps = list(pre)
        cuts, at = [], 0
        for a, b in p['protect']:
            cuts.append(('out', ''.join(cps[at:a])))
            cuts.append(('in', ''.join(cps[a:b])))
            at = b
        cuts.append(('out', ''.join(cps[at:])))
        p['cuts'] = cuts
        for i, (kind, t) in enumerate(cuts):
            if kind == 'out':
                feed.append(t)
                where.append((p['line'], i))
    out, msgs = original.perl(feed)
    require(len(out) == len(feed), 'perl line count')
    result, messages, multi = [], {}, set()
    k = 0
    for p in pieces:
        parts = []
        for i, (kind, t) in enumerate(p['cuts']):
            if kind == 'out':
                parts.append(out[k])
                for m in msgs.get(k + 1, []):
                    messages.setdefault(p['line'], set()).add(m)
                k += 1
            else:
                parts.append(t)
        if len(p['cuts']) > 1:
            multi.add(p['line'])
        result.append(''.join(parts))
    return result, messages, multi


def log_by_line(log):
    by = {}
    for line in log:
        m = re.match(r'^(\d+)行目(.*)$', line)
        require(m, f'log line {line!r}')
        by.setdefault(int(m.group(1)), []).append(m.group(2))
    return by


def strip_end(lines):
    return lines[:-1] if lines and lines[-1] == '' else lines


# ---- 1: rules 1-5 and 7 against the scripts on hand-made lines ---------------
PROBES = [
    'あ！', 'あ！い', 'あ！！', 'あ！！！', 'あ！！！！', 'あ！！！！！', 'あ！？', 'あ？！', 'あ！？！', '「あ！」', '『あ！』',
    '（あ！）', '「あ？」', '『あ？』', '（あ？）', 'あ？', 'あ？い', 'あ　　い', 'あ　　　い', 'あ　　　　い', 'あ・・・い',
    'あ・・・・い', 'あ・・・・・・い', 'あ・・い', 'あ！　い', 'あ！　', 'あ！　　', '！', '？', '！！', '　！', '！」！',
    '!?ab 1', 'a!b', '(a!)', '｜漢字《かん！じ》です！', '漢字《かんじ・・・》', '《《傍点！》》だ！', '｜《エスケープ！》',
    '漢字(かんじ)！', '|親《おや》', '魔法《magic》', '魔法《ma gic》', 'a b c', ' 先頭', '末尾 ', '　◇', '◇◇◇', '＊＊＊',
    '――', '「……」', '……', '', '　', '漢字《！》！', '｜漢字！《かんじ》', '漢字《かんじ》！」', '！《《！》》！',
    '｜あ《い》｜う《え》！お《か》', 'ＡＢＣ！ＤＥＦ？', '<&>"\'', '~', '\\', '　　　', '！　！　！',
]


def check_probes(tool, original):
    src = tool.work / 'probes.txt'
    src.write_text('\n'.join(PROBES) + '\n', encoding='utf-8')
    count = 0
    for rule, half in ((ORIGINAL, False), (ORIGINAL7, True)):
        pieces = tool.pieces(rule, src)
        for p, line in zip(pieces, PROBES):
            mine = han2zen(line)
            if half:
                mine = mine.replace(' ', '')
            require(p['pre'] == mine, f'probe {line!r}: ZENKAKU/HALFSPACE {p["pre"]!r} != {mine!r}')
        want, wmsg, _ = expected_by_pieces(original, pieces[:len(PROBES)], half)
        got, _, log, _ = tool.convert('format', rule, src)
        got = strip_end(got)
        require(len(got) == len(want), f'probe lines {len(got)} {len(want)}')
        by = log_by_line(log)
        for i, (g, w, line) in enumerate(zip(got, want, PROBES)):
            require(g == w, f'probe {line!r} rules {rule}: {g!r} != scripts {w!r}')
            mine = set(m for m in by.get(i + 1, []) if m not in ('の半角を全角に修正', 'の半角の空白を除きました'))
            require(mine == wmsg.get(i + 1, set()), f'probe {line!r}: messages {mine} != {wmsg.get(i + 1)}')
            count += 1
        # Ruby text keeps its ！ and ・・・; the half-width bar and parentheses become full width.
        require(got[PROBES.index('｜漢字《かん！じ》です！')] == '｜漢字《かん！じ》です！', 'ruby text untouched')
        require(got[PROBES.index('漢字《かんじ・・・》')] == '漢字《かんじ・・・》', 'ruby leader untouched')
        require(got[PROBES.index('《《傍点！》》だ！')] == '《《傍点！》》だ！', 'emphasis untouched')
        require(got[PROBES.index('|親《おや》')] == '｜親《おや》', 'bar to full width')
        require(got[PROBES.index('魔法《magic》')] == '魔法《ｍａｇｉｃ》', 'ruby letters to full width')
        require(got[PROBES.index('漢字(かんじ)！')] == '漢字（かんじ）！', 'parentheses to full width')
        require(got[PROBES.index('｜漢字！《かんじ》')] == '｜漢字！　《かんじ》', 'the base of a bar ruby is outside 《》')
    return count


# ---- 2: episodes ---------------------------------------------------------------
def episode_files(full):
    files = sorted((NOVELS / WORK_NAME).glob('episode_*.txt'))
    if not full:
        return files[:4] + files[-2:]
    for work in sorted(NOVELS.iterdir()):
        if work.is_dir() and work.name != WORK_NAME:
            files += sorted(work.glob('episode_*.txt'))[:5]
    return files


def copy_episodes(work, full):
    out = []
    for i, f in enumerate(episode_files(full)):
        d = work / 'episodes' / f'{i:04d}'
        d.mkdir(parents=True, exist_ok=True)
        crlf = d / f.name
        shutil.copyfile(f, crlf)
        lf = d / ('lf_' + f.name)
        lf.write_bytes(crlf.read_bytes().replace(b'\r\n', b'\n'))
        out.append((crlf, lf))
    return out


def check_episodes(tool, original, episodes):
    totals = {'files': 0, 'lines': 0, 'ruby_lines': 0, 'pipeline_diff': 0, 'pipeline_diff_lines': [],
              'crlf_files': 0, 'scene_lines': 0}
    for crlf, lf in episodes:
        text = lf.read_text(encoding='utf-8')
        lines = strip_end(text.split('\n'))
        totals['files'] += 1
        totals['lines'] += len(lines)
        if b'\r\n' in crlf.read_bytes():
            totals['crlf_files'] += 1
        # (a) FDTP's order with the scripts: rules 1-5 and 7.
        pieces = tool.pieces(ORIGINAL7, lf)
        for p, line in zip(pieces, lines):
            mine = han2zen(line)
            if not p['header']:
                mine = mine.replace(' ', '')
            require(p['pre'] == mine, f'{lf.name}:{p["line"]}: pre {p["pre"]!r} != {mine!r}')
        want, wmsg, multi = expected_by_pieces(original, pieces[:len(lines)], True)
        totals['ruby_lines'] += len(multi)
        got, raw, log, _ = tool.convert('format', ORIGINAL7, lf)
        got = strip_end(got)
        if got != want:
            first = next(((i + 1, g, w) for i, (g, w) in enumerate(zip(got, want)) if g != w), (len(got), len(want)))
            raise Failure(f'{lf.name}: rules 1-5,7 differ from the scripts at {first}')
        by = log_by_line(log)
        for n in range(1, len(lines) + 1):
            mine = [m for m in by.get(n, []) if m not in ('の半角を全角に修正', 'の半角の空白を除きました')]
            require(set(mine) == wmsg.get(n, set()), f'{lf.name}:{n}: messages {mine} != {wmsg.get(n)}')
        # (b) The original pipeline as it is: perl on the whole file, then han2zen.
        plines, pmsg = original.perl(lines)
        piped = original.han2zen(('\n'.join(plines) + '\n').encode('utf-8')).decode('utf-8')
        piped = strip_end(piped.split('\n'))
        g2, _, log2, _ = tool.convert('format', ORIGINAL, lf)
        g2 = strip_end(g2)
        require(len(g2) == len(piped), f'{lf.name}: pipeline line count')
        by2 = log_by_line(log2)
        for i, (g, w) in enumerate(zip(g2, piped)):
            if g != w:
                # Allowed only where the order differs (half-width !? turned full
                # width first) or text inside 《》 is protected.
                reason = re.search(r'[!?]', lines[i]) or (i + 1) in multi
                require(reason, f'{lf.name}:{i + 1}: pipeline {w!r} != {g!r}')
                totals['pipeline_diff'] += 1
                totals['pipeline_diff_lines'].append(f'{lf.parent.name}/{lf.name}:{i + 1}')
            else:
                mine = [m for m in by2.get(i + 1, []) if m != 'の半角を全角に修正']
                require(mine == pmsg.get(i + 1, []), f'{lf.name}:{i + 1}: history {mine} != {pmsg.get(i + 1)}')
        # (c) The CRLF original: the same lines, CRLF kept on every line.
        g3, raw3, _, _ = tool.convert('format', ORIGINAL7, crlf)
        if b'\r\n' in crlf.read_bytes():
            require(raw3.count(b'\n') == raw3.count(b'\r\n'), f'{crlf.name}: CRLF kept')
        require(strip_end(g3) == got, f'{crlf.name}: CRLF and LF give the same lines')
        # (d) Everything on: a scene break stays as written, the body is indented.
        g4, _, _, _ = tool.convert('format', ALL, crlf)
        pieces4 = tool.pieces(ALL, crlf)
        for p, g in zip(pieces4, g4):
            if p['scene']:
                totals['scene_lines'] += 1
                require(g == p['pre'], f'{crlf.name}:{p["line"]}: scene line kept {g!r}')
            elif not p['header'] and g:
                require(g[0] in '　「『（', f'{crlf.name}:{p["line"]}: indented {g[:5]!r}')
            require(' ' not in g or p['header'] or p['scene'], f'{crlf.name}:{p["line"]}: half-width space {g!r}')
            require(not re.search(r'[!-~]', g), f'{crlf.name}:{p["line"]}: half-width {g!r}')
    return totals


# ---- 3: rules 6, 8-12 -------------------------------------------------------------
CASES = [
    # (input, rules, expected)
    ('あ・・・い', ALL, '　あ……い'),
    ('あ・・・い', rules('LEADER'), 'あ…い'),
    ('あ・・・・・・い', ALL, '　あ…………い'),
    ('あ…い', ALL, '　あ……い'),
    ('あ………い', ALL, '　あ…………い'),
    ('あ……い', ALL, '　あ……い'),
    ('あ・・い', ALL, '　あ……い'),
    ('あ...い', ALL, '　あ……い'),
    ('あ.い', ALL, '　あ．い'),
    ('あ・・・・い', ALL, '　あ…………い'),
    ('ジョン・スミス', ALL, '　ジョン・スミス'),
    ('あ…い', rules('LEADER_EVEN'), 'あ……い'),
    ('あ…い', rules(*[r for r in RULES if r != 'LEADER_EVEN']), '　あ…い'),
    ('あ――い', ALL, '　あ――い'),
    ('あ―い', ALL, '　あ――い'),
    ('あ———い', ALL, '　あ――――い'),
    ('漢ーー字', ALL, '　漢――字'),
    ('ァーーー！', ALL, '　ァーーー！'),
    ('コーヒー――', ALL, '　コーヒー――'),
    ('コーヒー—', ALL, '　コーヒー――'),
    ('A-1', ALL, '　Ａ－１'),
    ('a--b', ALL, '　ａ――ｂ'),
    ('漢─────', ALL, '　漢――――――'),
    ('ーー', ALL, '　――'),
    ('超ー', ALL, '　超ー'),
    ('あ―い', rules(*[r for r in RULES if r != 'DASH']), '　あ―い'),
    ('「はい。」', ALL, '「はい」'),
    ('『はい。』', ALL, '『はい』'),
    ('（はい。）', ALL, '（はい。）'),
    ('はい。', ALL, '　はい。'),
    ('「はい。」', rules(*[r for r in RULES if r != 'PERIOD']), '「はい。」'),
    ('地の文', ALL, '　地の文'),
    ('「台詞」', ALL, '「台詞」'),
    ('『題』', ALL, '『題』'),
    ('（注）', ALL, '（注）'),
    ('(注)', ALL, '（注）'),
    ('　既に', ALL, '　既に'),
    (' 半角先頭', ALL, '　半角先頭'),
    ('地の文', rules(*[r for r in RULES if r != 'INDENT']), '地の文'),
    ('a b', ALL, '　ａｂ'),
    ('a b', rules(*[r for r in RULES if r != 'HALFSPACE']), '　ａ ｂ'),
    ('　◇', ALL, '　◇'),
    ('◇', ALL, '◇'),
    ('◇ ◇', ALL, '◇ ◇'),
    ('＊＊＊', ALL, '＊＊＊'),
    ('*', ALL, '＊'),
    ('――', ALL, '――'),
    ('※', ALL, '※'),
    ('◇◇◇◇◇◇◇◇◇◇', ALL, '◇◇◇◇◇◇◇◇◇◇'),
    ('◇◇◇◇◇◇◇◇◇◇◇', ALL, '　◇◇◇◇◇◇◇◇◇◇◇'),
    ('!!!', ALL, '　！！！'),
    ('・・・', ALL, '　……'),
    ('「……」', ALL, '「……」'),
    ('◇あ', ALL, '　◇あ'),
    ('◇1', ALL, '　◇１'),
    ('◇・・・', ALL, '　◇……'),
    ('　◇', rules(*[r for r in RULES if r != 'SCENE']), '　◇'),
    ('◇', rules(*[r for r in RULES if r != 'SCENE']), '　◇'),
    ('あ！い', rules(*[r for r in RULES if r != 'EXCLAIM']), '　あ！い'),
    ('あ！', rules(*[r for r in RULES if r != 'EXCLAIM_KEEP']), '　あ！　'),
    ('あ　　い', rules(*[r for r in RULES if r != 'SPACES']), '　あ　　い'),
    ('a!', rules(*[r for r in RULES if r != 'ZENKAKU']), '　a!'),
    ('あ・・・い', rules(*[r for r in RULES if r not in ('LEADER', 'LEADER_EVEN')]), '　あ・・・い'),
    ('漢字《かんじ。」》', ALL, '　漢字《かんじ。」》'),
    ('漢字《―》―', ALL, '　漢字《―》――'),
    ('《《傍点…》》…', ALL, '　《《傍点…》》……'),
    ('', ALL, ''),
    ('　', ALL, '　'),
    ('「あ！」・・・', ALL, '「あ！」……'),
]


def check_cases(tool):
    text = '\n'.join(c[0] for c in CASES)
    count = 0
    by_rule = {}
    for i, (line, rule, want) in enumerate(CASES):
        by_rule.setdefault(rule, []).append((i, line, want))
    for rule, items in by_rule.items():
        lines, log = tool.fmt('\n'.join(x[1] for x in items) + '\n', rule)
        for k, (i, line, want) in enumerate(items):
            require(lines[k] == want, f'case {line!r} rules {rule}: {lines[k]!r} != {want!r}')
            count += 1
    del text
    return count


def check_header(tool):
    head = ('【タイトル】\n第1話 x!\n\n【作成日時】\n2022-06-22 20:43:41（+09:00）\n\n【本文（3行）】\n'
            'a b!\n\n「x」\n')
    lines, log = tool.fmt(head, ALL)
    require(lines[:7] == ['【タイトル】', '第１話 ｘ！', '', '【作成日時】', '２０２２－０６－２２ ２０：４３：４１（＋０９：００）',
                          '', '【本文（３行）】'], f'header: rules 1-5 only {lines[:7]}')
    require(lines[7:] == ['　ａｂ！', '', '「ｘ」', ''], f'body after the header {lines[7:]}')
    # After formatting the header still reads as one (full-width digits).
    lines2, _, _, _ = tool.convert('web', None, '\n'.join(lines))
    require(lines2[:7] == lines[:7] and lines2[7:] == ['　ａｂ！', '', '「ｘ」', ''], f'header found again {lines2}')


# ---- 4: forms -----------------------------------------------------------------------
def check_forms(tool):
    n = 0
    src = 'a\n\nb\n\n\nc\n\n　◇\n\nd\n　◇\ne\n\n\n\nf\n'
    got, _, log, changed = tool.convert('manuscript', '1', src)
    want = ['a', 'b', '', 'c', '', '　◇', '', 'd', '', '　◇', '', 'e', '', 'f', '']
    require(got == want, f'manuscript {got}')
    require(log == ['2行目の空行を除きました', '4行目から空行が2行続いていたので一つにしました', '11行目の区切りの前に空行を入れました',
                    '11行目の区切りの後に空行を入れました', '13行目から空行が3行続いていたので一つにしました'], f'manuscript log {log}')
    require(changed == len(log), f'changed {changed}')
    got, _, _, _ = tool.convert('manuscript', '0', src)
    require(got == ['a', 'b', '', 'c', '　◇', 'd', '　◇', 'e', '', 'f', ''], f'manuscript without scenes {got}')
    got, _, _, _ = tool.convert('manuscript', '1', 'a\n　\nb\n \n\nc')
    require(got == ['a', 'b', '', 'c'], f'spaces-only lines are blank, no final line end kept {got}')
    got, _, log, changed = tool.convert('manuscript', '1', 'a\nb\n')
    require(got == ['a', 'b', ''] and changed == 0 and not log, f'nothing to do {got} {changed}')
    for text in ('', '\n', '\n\n\n', '　◇', '　◇\n', '\n　◇\n\n'):
        got, _, _, _ = tool.convert('manuscript', '1', text)
        n += 1
    got, _, _, _ = tool.convert('manuscript', '1', '\n　◇\n\n')
    require(got == ['', '　◇', '', ''], f'scene alone {got}')
    got, _, log, _ = tool.convert('web', None, 'a\nb\n\nc\nd')
    require(got == ['a', '', 'b', '', 'c', '', 'd'], f'web {got}')
    require(log == ['1行目の後に空行を入れました', '4行目の後に空行を入れました'], f'web log {log}')
    got, _, _, _ = tool.convert('web', None, 'a\nb\n')
    require(got == ['a', '', 'b', ''], f'web keeps the final line end {got}')
    # The header stays (its blank lines too), CRLF and Shift_JIS are kept.
    head = '【タイトル】\n題\n\n【本文（4行）】\n一\n\n二\n三\n'
    got, raw, _, _ = tool.convert('manuscript', '1', head, crlf=True, encoding='cp932')
    require(got == ['【タイトル】', '題', '', '【本文（4行）】', '一', '二', '三', ''], f'manuscript header {got}')
    require(raw.count(b'\r\n') == 7 and raw.count(b'\n') == 7, 'CRLF kept')
    got, raw, _, _ = tool.convert('web', None, head, crlf=True, encoding='cp932')
    require(got == ['【タイトル】', '題', '', '【本文（4行）】', '一', '', '二', '', '三', ''], f'web header {got}')
    require(raw.count(b'\r\n') == raw.count(b'\n'), 'CRLF kept (web)')
    return n


def check_episode_forms(tool, episodes):
    scenes = 0
    for crlf, _ in episodes:
        lines = strip_end(crlf.read_text(encoding='utf-8').replace('\r\n', '\n').split('\n'))
        body = next((i + 1 for i, l in enumerate(lines) if re.fullmatch(r'【本文（[0-9０-９]+行）】', l)), 0) \
            if lines and lines[0] == '【タイトル】' else 0
        man, raw, _, _ = tool.convert('manuscript', '1', crlf)
        require(raw.count(b'\n') == raw.count(b'\r\n'), f'{crlf.name}: CRLF kept')
        man = strip_end(man)
        require(man[:body] == lines[:body], f'{crlf.name}: header untouched')
        blank = lambda s: s.strip(' 　\t') == ''
        is_scene = lambda s: bool(re.fullmatch(r'[　 ]*[◇◆＊☆★※■□●○◎]{1,10}[　 ]*', s))
        require([l for l in man[body:] if not blank(l)] == [l for l in lines[body:] if not blank(l)],
                f'{crlf.name}: the text lines are kept in order')
        b = man[body:]
        for i, l in enumerate(b):
            if is_scene(l):
                scenes += 1
                require(i == 0 or b[i - 1] == '', f'{crlf.name}: blank before the scene break at {body + i + 1}')
                require(i + 1 == len(b) or b[i + 1] == '', f'{crlf.name}: blank after the scene break at {body + i + 1}')
            if l == '' and 0 < i < len(b) - 1:
                require(b[i + 1] != '', f'{crlf.name}: two blank lines left at {body + i + 1}')
        web, _, _, _ = tool.convert('web', None, write(crlf.parent / 'man.txt', raw))
        web = strip_end(web)
        wb = web[body:]
        require([l for l in wb if l] == [l for l in b if l], f'{crlf.name}: web keeps the text lines')
        for i, l in enumerate(wb):
            if l and i + 1 < len(wb):
                require(wb[i + 1] == '', f'{crlf.name}: web blank after {body + i + 1}')
    require(scenes > 0, 'episodes with scene breaks')
    return scenes


def write(path, data):
    path.write_bytes(data)
    return path


def check_undo(tool, episodes):
    for crlf, _ in episodes[:3]:
        r = json.loads(tool.run('undo', ALL, crlf))
        require(r['changed'] > 0 and r['undone'] and r['restored'] and r['redone'] and r['same'] and not r['more']
                and not r['modified'], f'undo: one C-z restores all {r}')


# ---- 5: Word -------------------------------------------------------------------------
def docx_parts(path):
    with zipfile.ZipFile(path) as z:
        return {i.filename: (z.read(i.filename), i.compress_type, i.CRC) for i in z.infolist()}, \
            [i.filename for i in z.infolist()]


def paragraph_text(p):
    """The text of a paragraph outside w:rt, tabs as \\t."""
    out = []

    def walk(e):
        if e.tag == W + 'rt':
            return
        if e.tag == W + 't':
            out.append(e.text or '')
        elif e.tag == W + 'tab':
            out.append('\t')
        for c in e:
            walk(c)
    walk(p)
    return ''.join(out)


def check_docx(tool, work, episodes):
    d = work / 'docx'
    d.mkdir(parents=True, exist_ok=True)
    template = d / '本文フォーマット.docx'
    shutil.copyfile(SCRIPTS / '本文フォ－マット.docx', template)
    tparts, torder = docx_parts(template)
    tdoc = tparts['word/document.xml'][0].decode('utf-8')
    tsect = re.search(r'<w:sectPr\b.*</w:sectPr>', tdoc, re.S).group(0)
    troot = tdoc[:tdoc.index('<w:body>')]
    sample = d / 'sample.txt'
    sample.write_text('【タイトル】\n第1話　題\n\n【本文（7行）】\n　｜漢字《かんじ》と草薙《くさなぎ》と漢字(かな)。\n\n'
                      '　《《傍点》》と｜《エスケープ》と<&>"\'\tタブ\x01\n　◇\n「台詞！」\n\n',
                      encoding='utf-8', newline='')
    made = {}
    for ruby in (0, 1):
        for src in [sample] + [c for c, _ in episodes[:2]]:
            out = d / f'{src.stem}-{"ruby" if ruby else "plain"}.docx'
            tool.run('docx', ruby, src, template, out)
            parts, order = docx_parts(out)
            require(order == torder, f'{out.name}: the same entries in the same order')
            for name, (data, method, crc) in tparts.items():
                if name != 'word/document.xml':
                    require(parts[name] == (data, method, crc), f'{out.name}: {name} copied byte for byte')
            xml = parts['word/document.xml'][0].decode('utf-8')
            require(xml.startswith(troot + '<w:body>'), f'{out.name}: namespaces as in the template')
            require(xml.endswith(tsect + '</w:body></w:document>'), f'{out.name}: sectPr as in the template')
            root = ET.fromstring(xml.encode('utf-8'))
            body = root.find(W + 'body')
            paras = body.findall(W + 'p')
            lines = strip_end(src.read_text(encoding='utf-8').replace('\r\n', '\n').split('\n'))
            hb = next(i + 1 for i, l in enumerate(lines) if l.startswith('【本文（'))
            want = [lines[1]] + lines[hb:]
            require(len(paras) == len(want), f'{out.name}: {len(paras)} paragraphs, {len(want)} lines')
            for p, line in zip(paras, want):
                plain = paragraph_text(p)
                if not ruby:
                    line = line.replace('\x01', '\ufffd')
                    require(plain == line, f'{out.name}: paragraph {plain!r} != {line!r}')
                    require(p.find('.//' + W + 'ruby') is None, f'{out.name}: no w:ruby by default')
            require(body.findall(W + 'sectPr') and body[-1].tag == W + 'sectPr', f'{out.name}: sectPr last')
            made[(src.name, ruby)] = (out, paras)
    _, plain = made[(sample.name, 0)]
    require('｜漢字《かんじ》' in ''.join(t.text or '' for t in plain[1].iter(W + 't')), 'notation kept as text')
    out, paras = made[(sample.name, 1)]
    rubies = paras[1].findall('.//' + W + 'ruby')
    require(len(rubies) == 3, f'three rubies {len(rubies)}')
    pairs = [(''.join(t.text for t in r.find(W + 'rubyBase').iter(W + 't')), ''.join(t.text for t in r.find(W + 'rt').iter(W + 't')))
             for r in rubies]
    require(pairs == [('漢字', 'かんじ'), ('草薙', 'くさなぎ'), ('漢字', 'かな')], f'ruby pairs {pairs}')
    pr = rubies[0].find(W + 'rubyPr')
    vals = {c.tag[len(W):]: c.get(W + 'val') for c in pr}
    require(vals == {'rubyAlign': 'distributeSpace', 'hps': '10', 'hpsRaise': '18', 'hpsBaseText': '21', 'lid': 'ja-JP'},
            f'rubyPr {vals}')
    text1 = ''.join(t.text or '' for t in paras[1].iter(W + 't'))
    require('《' not in text1 and '｜' not in text1 and '(' not in text1, f'notation gone {text1!r}')
    em = [r for r in paras[3].findall(W + 'r') if r.find(W + 'rPr/' + W + 'em') is not None]
    require(len(em) == 1 and em[0].find(W + 'rPr/' + W + 'em').get(W + 'val') == 'dot'
            and ''.join(t.text for t in em[0].iter(W + 't')) == '傍点', 'emphasis as w:em dot')
    text2 = ''.join(t.text or '' for t in paras[3].iter(W + 't'))
    require(text2 == '　傍点と《エスケープ》と<&>"\'タブ\ufffd' and paras[3].find('.//' + W + 'tab') is not None and paras[2].find(W + 'r') is None,
            f'escape and special characters {text2!r}')
    # A missing template.
    r = subprocess.run([str(tool.path), 'docx', '0', str(sample), str(d / 'none.docx'), str(d / 'x.docx')],
                       capture_output=True, text=True, env=tool.env)
    require(r.returncode == 1 and not (d / 'x.docx').exists(), 'missing template fails without output')
    return made


# ---- 6: the editor's keys ----------------------------------------------------------------
def keys(path, text):
    path.write_text(text, encoding='utf-8')
    return path


def check_editor(runner, work, template):
    d = work / 'editor'
    d.mkdir(parents=True, exist_ok=True)
    log = work / 'format.log'
    runner.env['FDTP_FORMAT_LOG'] = str(log)
    runner.env['FDTP_WORD_TEMPLATE'] = str(template)
    rc = runner.home / '.fdtprc'
    rc.parent.mkdir(parents=True, exist_ok=True)
    if rc.exists():
        rc.unlink()
    src = d / 'web.txt'
    original = '地の文・・・です！\n\n「台詞！」\n\n　◇\n\n次の場面\n'
    src.write_bytes(original.replace('\n', '\r\n').encode('utf-8'))
    # 1 choice box, 2 its y/n, 3 N leaves the text, 4 F2 again + y: the list,
    # 5 after the box, 6 C-z, 7 C-y.
    s = runner.run(src, keys(d / 'f1.keys', 'F2 SNAP RET SNAP "n" SNAP F2 RET "y" SNAP "x" SNAP C-z SNAP C-y SNAP C-s ESC\n'),
                   'fmt1')
    require(s[0]['widget'] == WIDGET_CHOICE and s[0]['widget_label'] == '変換' and s[0]['widget_rows'] ==
            ['出版社の書式に整形', '原稿形式へ（空行を除く）', 'Web 形式へ（空行を入れる）', 'Word 縦書きで書き出す'],
            f'choice box {s[0]["widget_label"]!r} {s[0]["widget_rows"]}')
    require(s[1]['widget'] == WIDGET_CONFIRM and s[1]['widget_label'] == '出版社の書式に整形しますか', f'y/n {s[1]["widget_label"]!r}')
    require(s[2]['widget'] == 0 and s[2]['state']['text'] == original.replace('\n', '\n'), 'N changes nothing')
    require(s[3]['widget'] == WIDGET_INFO and s[3]['widget_label'] == '出版社の書式に整形（8 件）', f'list {s[3]["widget_label"]!r}')
    rows = s[3]['widget_rows']
    require(rows[:3] == [['1行目', '・・・を……に修正'], ['1行目', '！を！　に修正'], ['1行目', '行末！だったので元に戻しました']]
            and rows[-1] == ['記録', str(log)], f'rows {rows}')
    # The publisher format is the manuscript form too: the single blank line
    # goes, the ones around the scene break stay.
    formatted = '　地の文……です！\n「台詞！」\n\n　◇\n\n　次の場面\n'
    require(s[4]['state']['text'] == formatted and s[4]['state']['modified'] == 1, f'formatted {s[4]["state"]["text"]!r}')
    require(s[5]['state']['text'] == original, 'one C-z restores everything')
    require(s[6]['state']['text'] == formatted, 'C-y again')
    require(src.read_bytes() == formatted.replace('\n', '\r\n').encode('utf-8'), 'saved with CRLF')
    text = log.read_text(encoding='utf-8')
    require(re.search(r'^HOST : .*\nfdtp  出版社の書式に整形\n\nExecuted time is \d\d/\d\d/\d\d \d\d:\d\d:\d\d\n', text, re.M)
            and f'PWD : {d.resolve()}\nChange TXT : web.txt\n1行目の・・・を……に修正\n' in text
            and text.endswith('=======================================================\n'), f'format.log {text[:600]!r}')
    # Manuscript and Web forms; nothing to do; a rule switched off in ~/.fdtprc.
    web_form = '　地の文……です！\n\n「台詞！」\n\n　◇\n\n　次の場面\n'
    src.write_bytes(web_form.replace('\n', '\r\n').encode('utf-8'))
    s = runner.run(src, keys(d / 'f2.keys', 'F2 DOWN RET SNAP "y" SNAP "x" F2 DOWN DOWN RET "y" SNAP "x" '
                                            'F2 DOWN DOWN RET "y" SNAP ESC "n"\n'), 'fmt2')
    require(s[0]['widget_label'] == '原稿形式に（空行を除く）しますか', f'manuscript y/n {s[0]["widget_label"]!r}')
    require(s[1]['state']['text'] == '　地の文……です！\n「台詞！」\n\n　◇\n\n　次の場面\n', f'manuscript {s[1]["state"]["text"]!r}')
    require(s[2]['state']['text'] == web_form, f'web {s[2]["state"]["text"]!r}')
    require(s[3]['widget'] == WIDGET_NOTICE and s[3]['notice'] == '直す所はありませんでした', f'nothing {s[3]["notice"]!r}')
    rc.write_text('set NOVEL_FORMAT_INDENT=0\nset NOVEL_FORMAT_LEADER2=0\nNOVEL_FORMAT_LEADER_EVEN=0\n', encoding='utf-8')
    plain = d / 'plain.txt'
    plain.write_text('地の文・・・\n', encoding='utf-8')
    s = runner.run(plain, keys(d / 'f3.keys', 'F2 RET "y" SNAP "x" SNAP ESC "n"\n'), 'fmt3')
    require(s[1]['state']['text'] == '地の文…\n', f'switches from ~/.fdtprc {s[1]["state"]["text"]!r}')
    # Word: the y/n says it cannot be undone; overwrite; ruby by NOVEL_WORD_RUBY.
    rc.write_text('', encoding='utf-8')
    novel = d / 'novel.txt'
    novel.write_text('　｜漢字《かんじ》です。\n', encoding='utf-8')
    dest = d / 'novel.docx'
    if dest.exists():
        dest.unlink()
    s = runner.run(novel, keys(d / 'w1.keys', 'F2 UP RET SNAP "y" SNAP F2 UP RET SNAP "n" SNAP ESC\n'), 'word1')
    require(s[0]['widget'] == WIDGET_CONFIRM and s[0]['widget_label'] ==
            'Word 縦書きで novel.docx に書き出しますか。\nこの処理は取り消し（Undo）が効きません', f'word y/n {s[0]["widget_label"]!r}')
    require(s[1]['notice'] == 'novel.docx に書き出しました（ルビ: 記法のまま）' and dest.exists(), f'written {s[1]["notice"]!r}')
    require(s[2]['widget_label'] == 'novel.docx は既にあります。上書きして書き出しますか。\nこの処理は取り消し（Undo）が効きません',
            f'overwrite {s[2]["widget_label"]!r}')
    require(s[1]['state']['modified'] == 0, 'the text is not changed by the export')
    with zipfile.ZipFile(dest) as z:
        require(b'w:ruby' not in z.read('word/document.xml'), 'no ruby by default')
    rc.write_text('set NOVEL_WORD_RUBY=1\n', encoding='utf-8')
    s = runner.run(novel, keys(d / 'w2.keys', 'F2 UP RET "y" SNAP ESC\n'), 'word2')
    require(s[0]['notice'] == 'novel.docx に書き出しました（ルビ: Word のルビ）', f'ruby notice {s[0]["notice"]!r}')
    with zipfile.ZipFile(dest) as z:
        require(b'<w:ruby>' in z.read('word/document.xml'), 'w:ruby with NOVEL_WORD_RUBY=1')
    runner.env['FDTP_WORD_TEMPLATE'] = str(d / 'missing.docx')
    s = runner.run(novel, keys(d / 'w3.keys', 'F2 UP RET "y" SNAP ESC\n'), 'word3')
    require(s[0]['notice'] == 'novel.docx に既定の雛形で書き出しました（ルビ: Word のルビ）', f'missing: the built-in one {s[0]["notice"]!r}')
    # The default template place: ~/.fdtp/template/本文フォーマット.docx.
    del runner.env['FDTP_WORD_TEMPLATE']
    t = runner.home / '.fdtp' / 'template'
    t.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(template, t / '本文フォーマット.docx')
    s = runner.run(novel, keys(d / 'w4.keys', 'F2 UP RET "y" SNAP ESC\n'), 'word4')
    require(s[0]['notice'].startswith('novel.docx に書き出しました'), f'default template {s[0]["notice"]!r}')
    # F1 lists F2.
    s = runner.run(novel, keys(d / 'h.keys', 'F1 SNAP ESC ESC\n'), 'help')
    rows = s[0]['widget_rows']
    require(['F2', '出版社の書式に整形'] in rows and ['', '原稿形式へ・Web 形式へ'] in rows and ['', 'Word 縦書きで書き出す'] in rows
            and [None, '変換（F2。どれも y/n で確かめてから）'] in rows, 'F2 in the help')
    rc.write_text('', encoding='utf-8')
    return runner.runs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--no-sanitize', action='store_true')
    parser.add_argument('--novels', type=Path, default=dev_env.get('FDTP_NOVELS_DIR'))
    parser.add_argument('--work', default=dev_env.get('FDTP_NOVEL_WORK'))
    args = parser.parse_args()
    if not args.novels or not args.work:
        print('SKIP: no episodes (set FDTP_NOVELS_DIR and FDTP_NOVEL_WORK, or --novels and --work)')
        return
    global NOVELS, WORK_NAME, SCRIPTS
    NOVELS, WORK_NAME = Path(args.novels).expanduser(), args.work
    SCRIPTS = NOVELS / WORK_NAME / 'script'
    directory = args.build.resolve()
    require(SCRIPTS.is_dir(), f'the user scripts {SCRIPTS}')
    variants = [('plain', directory, False)]
    if not args.no_sanitize:
        variants.append(('asan', directory / 'novel-asan', True))
    for label, build_dir, sanitize in variants:
        harness, tool_path = build(build_dir, sanitize)
        work = directory / 'novel-format-check' / label
        if work.exists():
            shutil.rmtree(work)
        (work / 'tmp').mkdir(parents=True)
        tool = Tool(tool_path, work)
        original = Original(work)
        probes = check_probes(tool, original)
        episodes = copy_episodes(work, full=not sanitize)
        totals = check_episodes(tool, original, episodes)
        cases = check_cases(tool)
        check_header(tool)
        check_forms(tool)
        scenes = check_episode_forms(tool, episodes)
        check_undo(tool, episodes)
        made = check_docx(tool, work, episodes)
        runner = cn.Runner(work, harness, harness, label)
        runs = check_editor(runner, work, work / 'docx' / '本文フォーマット.docx')
        print(f'{label}: probes {probes}, cases {cases}, episodes {totals["files"]} files {totals["lines"]} lines '
              f'({totals["crlf_files"]} CRLF, {totals["ruby_lines"]} lines with text in 《》, '
              f'{totals["scene_lines"]} scene lines), original pipeline differs on {totals["pipeline_diff"]} lines, '
              f'manuscript scene breaks {scenes}, docx {len(made)}, editor runs {runs}')
        if totals['pipeline_diff_lines']:
            print(f'{label}: pipeline differences at ' + ', '.join(totals['pipeline_diff_lines'][:20]))
        if label == 'plain':
            print('plain: Word files to look at: ' + ', '.join(str(made[(n, r)][0]) for n, r in sorted(made) if n == 'sample.txt'))


if __name__ == '__main__':
    try:
        main()
    except (Failure, subprocess.CalledProcessError, OSError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
