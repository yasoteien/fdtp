#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Stage 5 acceptance tests (docx): view, edit, save.

Builds tests/doc_harness.c with the sources of mk/doc.mk and zipfile.c (and
tests/ime_fake.c for the input method), makes the fixtures with
tools/make_test_office.py, opens them through the harness, checks the SNAP
cells, saves, and reads the saved file back with Python's zipfile and xml:
only the edited paragraphs may change; the other zip entries, the other
paragraphs and the root's namespace declarations must be byte-identical.
Everything is written under build/.

    python3 -B tests/check_doc.py --build build [--sanitize]
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile
import xml.etree.ElementTree as ET
import xml.parsers.expat
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('office', ROOT / 'tools/make_test_office.py')
office = importlib.util.module_from_spec(spec)
spec.loader.exec_module(office)
W = '{' + office.WML + '}'
SUPPORT = ['i18n', 'zipfile', 'app', 'widgets', 'keys', 'commands', 'canvas', 'cells', 'raster', 'font24', 'jis']
# Colours (src/cells.h).
BG, CYAN, WHITE, YELLOW = 0, 1, 2, 3
checks = 0


def require(condition, message):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)


def mk_sources():
    text = (ROOT / 'mk/doc.mk').read_text()
    return re.search(r'^DOC_SOURCES := (.*)$', text, re.M).group(1).split()


def build(build_dir, sanitize):
    (build_dir / 'tmp').mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(build_dir / 'tmp'))
    if not (build_dir / 'font24.inc').exists():
        subprocess.run([sys.executable, str(ROOT / 'tools/gen_font.py'), '--output', str(build_dir / 'font24.inc')], check=True, env=env)
    if not (build_dir / 'jis0208.inc').exists():
        subprocess.run([sys.executable, str(ROOT / 'tools/gen_jis.py'), '--output', str(build_dir / 'jis0208.inc')], check=True, env=env)
    # The harness compiles src/*.c directly, so keep the catalog in step with them.
    subprocess.run([sys.executable, str(ROOT / 'tools/gen_i18n.py'), '--output', str(build_dir / 'i18n_en.inc')], check=True, env=env)
    exe = build_dir / ('doc-test-asan' if sanitize else 'doc-test')
    sources = [str(ROOT / 'src' / (n + '.c')) for n in mk_sources() + SUPPORT]
    sources += [str(ROOT / 'tests/doc_harness.c'), str(ROOT / 'tests/ime_fake.c')]
    flags = ['-O1', '-g', '-std=c11', '-Wall', '-Wextra', '-Werror']
    if sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    libs = ['-lz', '-lexpat', '-lm']
    if sys.platform != 'darwin':
        libs = subprocess.run(['pkg-config', '--libs', 'zlib', 'expat'], capture_output=True, text=True, check=True).stdout.split() + ['-lm']
    subprocess.run([os.environ.get('CC', 'cc')] + flags + ['-I' + str(ROOT / 'src'), '-I' + str(build_dir)] + sources + ['-o', str(exe)] + libs,
                   check=True, env=env)
    return exe


class Harness:
    def __init__(self, exe):
        self.exe = exe

    def open(self, docx, script, prefix, expected=0, levels=None):
        env = dict(os.environ, FDTP_LANGUAGE='ja')
        env.pop('FDTP_UNDO_LEVELS', None)
        if levels is not None:
            env['FDTP_UNDO_LEVELS'] = str(levels)
        r = subprocess.run([str(self.exe), 'open', str(docx), str(script), str(prefix)], capture_output=True, env=env)
        out, err = r.stdout.decode('utf-8', 'replace'), r.stderr.decode('utf-8', 'replace')
        require(r.returncode == expected, f'{docx} {script}: exit {r.returncode}, expected {expected}\n{out}\n{err}')
        require('runtime error' not in err and 'AddressSanitizer' not in err and 'LeakSanitizer' not in err, err)
        snaps = []
        for line in out.splitlines():
            if line.startswith('SNAP '):
                serial = int(line.split()[1])
                snaps.append(json.loads(Path(f'{prefix}-{serial:03d}.cells').read_text()))
        return out, snaps


def row_text(snap, y):
    return ''.join(chr(c[0]) for c in snap['rows'][y] if c[3] != 2).rstrip()


def body_text(snap, y):
    """A row of the document: columns 1..78, inside the screen's frame."""
    return ''.join(chr(c[0]) for c in snap['rows'][y][1:79] if c[3] != 2).rstrip()


# Revision 1 item 29: the screen's frame over rows 0..22, the document in
# rows 1..21 and columns 1..78, the name in the top line, 「行 N/M」 in the
# bottom line (row 22), the item on row 23.
def screen(snap):
    return [body_text(snap, y) for y in range(1, 22)]


def status(snap):
    return row_text(snap, 22).strip()


def colors(snap, y):
    """Foreground colours of the characters of row y that are not spaces."""
    return {c[1] for c in snap['rows'][y] if c[3] != 2 and c[0] != 32}


def find_row(snap, text):
    for y in range(1, 22):
        if text in row_text(snap, y):
            return y
    return -1


def separators(snap):
    """The document's lines (page breaks): lines across the frame (box 0)."""
    require(snap['boxes'] and snap['boxes'][0][:4] == [0, 0, 80, 23] and snap['boxes'][0][5] == 1, f'the frame {snap["boxes"]}')
    return sorted(l[0] for l in snap['lines'] if l[3] == 0 and l[4] == 0 and l[1] == 0 and l[2] == 79)


def raw_entry(path, info):
    """Local header, data and descriptor of one zip entry, as stored."""
    data = path.read_bytes()
    start = info.header_offset
    name_len, extra_len = struct.unpack('<HH', data[start + 26:start + 30])
    end = start + 30 + name_len + extra_len + info.compress_size
    if info.flag_bits & 8:
        end += 16 if data[end:end + 4] == b'PK\x07\x08' else 12
    return data[start:end]


def body_children(data):
    """[(local name, bytes)] of the direct children of w:body, plus the bytes
    before the first and after the last."""
    p = xml.parsers.expat.ParserCreate(namespace_separator=' ')
    depth = 0
    out = []
    start = 0
    first = last = None

    def st(name, attrs):
        nonlocal depth, start, first
        depth += 1
        if depth == 3:
            start = p.CurrentByteIndex
            if first is None:
                first = start

    def en(name):
        nonlocal depth, last
        if depth == 3:
            i = p.CurrentByteIndex
            qn = re.match(rb'<([^\s/>]+)', data[start:]).group(1)
            if data[i:i + 2 + len(qn)] == b'</' + qn:
                end = data.index(b'>', i) + 1
            else:
                end = i
            out.append((name.split(' ')[-1], data[start:end]))
            last = end
        depth -= 1
    p.StartElementHandler = st
    p.EndElementHandler = en
    p.Parse(data, True)
    return out, data[:first], data[last:]


def para_text(p):
    out = []
    for e in p.iter():
        if e.tag == W + 't' and e.text:
            out.append(e.text)
        elif e.tag == W + 'tab':
            out.append('\t')
        elif e.tag == W + 'br':
            out.append('\f' if e.get(W + 'type') == 'page' else '\n')
    return ''.join(out)


def element(data):
    """Parses one body child with the w, r, w14 and wp prefixes bound."""
    ns = ' '.join(f'xmlns:{k}="{v}"' for k, v in (('w', office.WML), ('r', office.NS_REL), ('w14', 'http://schemas.microsoft.com/office/word/2010/wordml'),
                                                  ('wp', 'http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing')))
    return ET.fromstring(f'<x {ns}>'.encode() + data + b'</x>')[0]


def check_view(h, out):
    d = out / 'view'
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    docx = office.make_docx(d / 'doc.docx')
    (d / 'v.keys').write_text('SNAP END SNAP HOME DOWN DOWN DOWN DOWN DOWN DOWN SNAP\n')
    _, s = h.open(docx, d / 'v.keys', d / 'v')
    first = s[0]
    rows = screen(first)
    require('doc.docx' in row_text(first, 0) and status(first) == '行 1/30', (row_text(first, 0), status(first)))
    require(first['frame'] == 0, 'full screen without the FD frame')
    require(rows[0] == '第一章　はじめに' and colors(s[2], 1) == {YELLOW}, 'heading 1 (style 1 named "heading 1") in YELLOW')
    # The cursor line is reversed (inside the frame).
    require(all(c[2] == WHITE for c in first['rows'][1][1:79]), 'cursor line reversed')
    require(rows[1] == 'これは本文の段落です。' and colors(s[2], 2) == {WHITE}, 'body in WHITE')
    require(rows[2] == '太字と普通 mixed & <記号>', 'runs joined, bold not marked, entities decoded')
    require(rows[3] == 'ブックマークの段落' and rows[4] == 'ページ 1', 'bookmark and field result shown')
    table = rows[5:13]
    require(table[0] == '┌───┬──────┬──┐', f'table top border {table[0]!r}')
    require(table[1].startswith('│品名') and table[1].count('│') == 4, table[1])
    require(table[3].startswith('│りんご│赤い果物') and table[4].startswith('│      │二つ目の段落'), 'cell paragraphs on lines of their own')
    require(table[5] == '├───┼──────┴──┤', f'span border {table[5]!r}')
    require(table[6].startswith('│結合  │二列にまたがる') and table[6].count('│') == 3, table[6])
    require(table[7] == '└───┴─────────┘', table[7])
    require(colors(first, 6) == {CYAN} and colors(s[2], 7) == {BG}, 'ruled lines in CYAN, reversed under the cursor')
    require(separators(first) == [14, 16], f'page break and page-break-before lines {separators(first)}')
    require(rows[13] == '' and rows[14] == '図：[画像]', 'image')
    y = find_row(first, '[画像]')
    require(any(c[1] == CYAN for c in first['rows'][y][5:11]) and first['rows'][y][1][1] == WHITE, '[画像] in CYAN')
    require(rows[16] == '第二章' and colors(s[2], 17) == {YELLOW}, 'style based on heading 1 is a heading')
    require(rows[17] == 'コメント付き' and rows[18] == 'リンク: example', 'comment, link text')
    long_lines = rows[19:21]
    require(all(sum(2 if ord(ch) > 0x7f else 1 for ch in line) <= 78 for line in long_lines), 'wrapped at 78 columns')
    require(''.join(long_lines) == office.LONG_TEXT[:len(''.join(long_lines))], 'wrapping keeps every character')
    require(row_text(first, 23).strip() == '段落 1/17  見出し', row_text(first, 23))
    band = row_text(first, 24)
    require('Edit' in band and 'Open' in band and 'Kill' in band and 'Save' in band and 'Quit' in band, band)
    require(first['rows'][24][3][2] == YELLOW and first['rows'][24][3][0] == ord('E'), 'band key letter on yellow')
    end = s[1]
    require(status(end) == '行 30/30', 'END goes to the last line')
    last = screen(end)
    require(last[-1] == '最後の段落' and all(c[2] == WHITE for c in end['rows'][21][1:79]), 'last line under the cursor')
    require('A   B' in last and 'C' in last, 'tab expanded, line break')
    require(separators(end) and screen(end)[separators(end)[-1] - 1] == '', 'section break line')
    require(row_text(s[2], 23).strip() == '表 6/17', 'table item on row 23')
    return docx


def check_edit(h, out):
    d = out / 'edit'
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    docx = office.make_docx(d / 'doc.docx')
    original = docx.read_bytes()
    shutil.copy(docx, d / 'original.docx')
    os.chmod(docx, 0o640)
    stdout, s = h.open(docx, ROOT / 'tests/doc_edit.keys', d / 'e')
    require(stdout.strip().endswith('END 0') and len(s) == 25, f'{len(s)} snapshots\n{stdout}')
    notices = [x['notice'] for x in s]
    input_row = row_text(s[1], 23)
    require(input_row.startswith('  段落: これは本文の段落です。'), input_row)
    require(s[1]['rows'][23][2][1] == CYAN, 'input label in CYAN')
    require(screen(s[2])[1] == '書き換えた本文です', 'body replaced')
    require(screen(s[3])[2] == '太字と普通 mixed & <記号>!', 'mixed')
    require(screen(s[4])[3] == 'ブックマークの段落 直した', 'bookmark paragraph')
    require(notices[5] == 'ブックマークやコメントの範囲を含むため、この段落は消せません', notices[5])
    require(notices[6] == 'フィールドを含むため、この段落は直せません', notices[6])
    require(notices[7] == '表の中は直せません' and notices[8] == '表は消せません', notices[7:9])
    require(screen(s[9])[14] == '図１：[画像]', screen(s[9])[14])
    sc = screen(s[10])
    require(sc[16] == '第二章' and sc[17] == '第三節' and colors(s[10], 17) == {YELLOW} and separators(s[10]) == [14, 16],
            'o: new paragraph after heading 2, heading style, no extra page break')
    require(row_text(s[10], 23).strip() == '段落 10/18  見出し', row_text(s[10], 23))
    require(screen(s[11])[18] == 'コメント付き（直）', 'comment paragraph')
    require(notices[12] == 'リンクを含むため、この段落は直せません', notices[12])
    scrolled = row_text(s[13], 23)
    require(scrolled.startswith('  段落: ') and scrolled.endswith('ここで終わり。') and 'これは' not in scrolled, f'row 23 scrolled: {scrolled!r}')
    require(any(c[2] == WHITE for c in s[13]['rows'][23][60:]), 'input caret at the end')
    require(not notices[14] and status(s[14]) == '行 21/31' and row_text(s[14], 0).endswith('変更あり'), 'long paragraph edited')
    require(notices[15] == '変更履歴を含むため、この段落は直せません', notices[15])
    asked = ''.join(row_text(s[16], y).strip() for y in range(1, 23))  # the confirmation box (revision 1 §7)
    require(s[16]['widget'] == 2 and 'この段落を消しますか？ (Y/N)' in asked, 'delete asks')
    require('A   B' in screen(s[17]), 'n keeps the paragraph')
    require('A   B' not in screen(s[18]) and 'C' not in screen(s[18]), 'y deletes it')
    require('空だった段落' in screen(s[19]), 'empty paragraph edited')
    require(notices[20] == '文書の最後の段落は消せません', notices[20])
    require('消える' not in ''.join(screen(s[21])) and row_text(s[21], 0).endswith('変更あり'), 'o then Esc adds nothing')
    require(screen(s[22])[-1] == '日本go' and row_text(s[22], 23).strip() == '段落 18/18', 'input method')
    require(notices[23] == '保存しました（元のファイルは .bak に残しました）', notices[23])
    require(not row_text(s[23], 0).endswith('変更あり'), 'not dirty after saving')
    require(s[24]['depth'] == 0, 'Esc closes')
    # ---- the saved file ----
    bak = Path(str(docx) + '.bak')
    require(bak.read_bytes() == original, '.bak is the original')
    require((docx.stat().st_mode & 0o777) == 0o640, 'file mode kept')
    require(not [p for p in d.iterdir() if p.name.startswith('.doc.docx')], 'no temporary file left')
    with zipfile.ZipFile(docx) as z, zipfile.ZipFile(d / 'original.docx') as o:
        require(z.testzip() is None, 'CRCs')
        require(z.namelist() == o.namelist(), 'same entries in the same order')
        for name in o.namelist():
            if name != 'word/document.xml':
                require(raw_entry(docx, z.getinfo(name)) == raw_entry(d / 'original.docx', o.getinfo(name)), f'{name} copied byte for byte')
        require(z.getinfo('word/document.xml').compress_type == o.getinfo('word/document.xml').compress_type, 'method kept')
        new, old = z.read('word/document.xml'), o.read('word/document.xml')
    ET.fromstring(new)
    a, a_head, a_tail = body_children(old)
    b, b_head, b_tail = body_children(new)
    require(a_head == b_head, 'declaration and root start tag (namespace declarations) byte-identical')
    require(a_tail == b_tail, 'body sectPr and the end byte-identical')
    labels = [label for label, _ in office.docx_body()]
    olds = dict(zip(labels, [x for _, x in a]))
    expected = ['heading', 'body*', 'mixed*', 'bookmark*', 'field', 'table', 'pagebreak', 'image*', 'heading2', '+',
                'comment*', 'link', 'long*', 'tracked', 'empty*', 'section', 'last', '+']
    require(len(b) == len(expected) + 1, f'{len(b)} body children')
    got = dict()
    for want, (kind, data) in zip(expected, b):
        if want == '+':
            require(kind == 'p', 'added paragraph')
            continue
        label = want.rstrip('*')
        got[label] = data
        if want.endswith('*'):
            require(data != olds[label], f'{label} changed')
        else:
            require(data == olds[label], f'{label} byte-identical')
    require(b[-1][0] == 'sectPr' and b[-1][1] == a[-1][1], 'body sectPr kept')
    require(b'<w:r><w:t>A</w:t><w:tab/>' not in new, 'deleted paragraph gone')

    def runs(p):
        return [r for r in p if r.tag == W + 'r']
    body = element(got['body'])
    require(got['body'].startswith(olds['body'][:olds['body'].index(b'<w:r ')]), 'start tag (w14:paraId) and pPr kept')
    require(b'<w:r w:rsidRPr="00B20002"><w:rPr><w:rFonts w:hint="eastAsia"/></w:rPr>' in got['body'], 'first run start tag and rPr kept')
    require(len(runs(body)) == 1 and para_text(body) == '書き換えた本文です', 'body text')
    mixed = element(got['mixed'])
    require(len(runs(mixed)) == 1 and para_text(mixed) == '太字と普通 mixed & <記号>!', para_text(mixed))
    require(runs(mixed)[0].find(W + 'rPr').find(W + 'b') is not None and runs(mixed)[0].find(W + 'rPr').find(W + 'i') is None,
            'the first run\'s rPr (bold) only')
    require(len(mixed.findall(W + 'proofErr')) == 2, 'proofErr kept')
    require(b'&amp; &lt;' in got['mixed'], 'text escaped')
    bm = element(got['bookmark'])
    require([e.tag.split('}')[1] for e in bm] == ['bookmarkStart', 'r', 'bookmarkEnd'], 'bookmark around the run')
    require(b'<w:bookmarkStart w:id="0" w:name="mark1"/>' in got['bookmark'] and para_text(bm) == 'ブックマークの段落 直した', 'bookmark kept')
    image = element(got['image'])
    require(para_text(image) == '図１：' and len(runs(image)) == 2, 'image paragraph text')
    drawing = olds['image'][olds['image'].index(b'<w:r><w:rPr><w:noProof/>'):-len(b'</w:p>')]
    require(drawing in got['image'], 'drawing run byte-identical')
    cm = element(got['comment'])
    require([e.tag.split('}')[1] for e in cm] == ['commentRangeStart', 'r', 'commentRangeEnd', 'r'], 'comment range order')
    require(b'<w:r><w:rPr><w:rStyle w:val="CommentReference"/></w:rPr><w:commentReference w:id="0"/></w:r>' in got['comment'], 'comment reference kept')
    require(para_text(cm) == 'コメント付き（直）', para_text(cm))
    require(para_text(element(got['long'])) == office.LONG_TEXT + '追記', 'long text')
    require(got['empty'] == '<w:p w:rsidR="00A10001"><w:r><w:t xml:space="preserve">空だった段落</w:t></w:r></w:p>'.encode(), got['empty'])
    added = [data for want, (kind, data) in zip(expected, b) if want == '+']
    require(added[0] == '<w:p><w:pPr><w:pStyle w:val="MyHead"/></w:pPr><w:r><w:t xml:space="preserve">第三節</w:t></w:r></w:p>'.encode(), added[0])
    require(added[1] == '<w:p><w:r><w:t xml:space="preserve">日本go</w:t></w:r></w:p>'.encode(), added[1])
    shutil.copy(docx, d / 'edited.docx')   # for the look in Word (main session)
    # ---- opened again: shown, edited and saved a second time ----
    stdout, v = h.open(docx, ROOT / 'tests/doc_view.keys', d / 'v')
    require(screen(v[0])[1] == '書き換えた本文です' and screen(v[0])[17] == '第三節', 'saved file opens')
    require('doc.docx' in row_text(v[1], 0) and screen(v[1]) != screen(v[0]), 'PageDown')
    require(screen(v[2]) == screen(v[0]), 'PageUp back')
    require(screen(v[3])[1] == '二度目', 'edited again')
    asked = ''.join(row_text(v[4], y).strip() for y in range(1, 23))
    require(v[4]['widget'] == 2 and '保存せずに戻りますか？' in asked, 'asks before leaving')
    require(v[5]['notice'] == '保存しました（元のファイルは .bak に残しました）', v[5]['notice'])
    require(v[6]['depth'] == 0, 'q closes')
    with zipfile.ZipFile(docx) as z:
        again = z.read('word/document.xml')
    c, _, _ = body_children(again)
    require(para_text(element(c[1][1])) == '二度目' and [x for _, x in c[2:]] == [x for _, x in b[2:]], 'second save changes one paragraph')
    require(Path(str(docx) + '.bak').read_bytes() != original, 'second session: .bak is the file as it was opened')
    return docx


def check_broken(h, out):
    d = out / 'broken'
    shutil.rmtree(d, ignore_errors=True)
    office.make_broken_docx(d)
    (d / 'k.keys').write_text('SNAP\n')
    for name in ('notzip.docx', 'nodocument.docx', 'badxml.docx', 'notword.docx', 'truncated.docx', 'missing.docx'):
        stdout, _ = h.open(d / name, d / 'k.keys', d / name, expected=3)
        require(stdout.startswith('NOTICE 文書を開けません'), f'{name}: {stdout!r}')


def check_strange(h, out):
    """Unusual but valid: a different prefix, a default namespace, a BOM,
    paragraphs inside a content control, an absolute rels target."""
    d = out / 'strange'
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    doc = (office.XML_DECL + f'<x:document xmlns:x="{office.WML}"><x:body>'
           '<x:sdt><x:sdtPr><x:alias x:val="c"/></x:sdtPr><x:sdtContent><x:p><x:r><x:t>中身</x:t></x:r></x:p></x:sdtContent></x:sdt>'
           '<x:p><x:r><x:t>二つ目</x:t></x:r></x:p><x:p><x:r><x:t>三つ目</x:t></x:r></x:p></x:body></x:document>')
    docx = office.make_docx(d / 'prefix.docx', '﻿' + doc)
    (d / 'k.keys').write_text('SNAP RET C-u "直した" RET o "足した" RET C-s SNAP\n')
    _, s = h.open(docx, d / 'k.keys', d / 'p')
    require(screen(s[0])[:3] == ['中身', '二つ目', '三つ目'], screen(s[0])[:3])
    require(s[1]['notice'].startswith('保存しました'), s[1]['notice'])
    with zipfile.ZipFile(docx) as z:
        new = z.read('word/document.xml')
    require(new.startswith(b'\xef\xbb\xbf'), 'BOM kept')
    require('<x:sdtContent><x:p><x:r><x:t xml:space="preserve">直した</x:t></x:r></x:p><x:p><x:r><x:t xml:space="preserve">足した</x:t></x:r></x:p></x:sdtContent>'.encode() in new,
            'the paragraph\'s own prefix used, insertion inside the content control')
    require(new.endswith('<x:p><x:r><x:t>二つ目</x:t></x:r></x:p><x:p><x:r><x:t>三つ目</x:t></x:r></x:p></x:body></x:document>'.encode()), 'rest unchanged')
    # Default namespace (no prefix) and an absolute target in _rels/.rels.
    doc = office.XML_DECL + f'<document xmlns="{office.WML}"><body><p><r><t>既定</t></r></p><p/></body></document>'
    parts = office.docx_parts(doc)
    parts = [(n, x.replace('Target="word/document.xml"', 'Target="/word/document.xml"') if n == '_rels/.rels' else x) for n, x in parts]
    docx = office.package(d / 'default.docx', parts)
    (d / 'k2.keys').write_text('SNAP RET C-u "名前空間" RET C-s SNAP\n')
    _, s = h.open(docx, d / 'k2.keys', d / 'q')
    require(screen(s[0])[0] == '既定' and s[1]['notice'].startswith('保存しました'), 'default namespace document')
    with zipfile.ZipFile(docx) as z:
        new = z.read('word/document.xml')
    require(b'<p><r><t xml:space="preserve">\xe5\x90\x8d\xe5\x89\x8d\xe7\xa9\xba\xe9\x96\x93</t></r></p><p/>' in new, new)
    ET.fromstring(new)
    # Tabs, line and page breaks and a non-breaking hyphen survive an edit.
    doc = (office.XML_DECL + office.DOCX_ROOT + '<w:body><w:p><w:r><w:rPr><w:sz w:val="28"/></w:rPr><w:t>A</w:t><w:tab/><w:t>B</w:t>'
           '<w:br/><w:t>C</w:t><w:br w:type="page"/><w:t>D</w:t><w:noBreakHyphen/><w:t>E</w:t><w:lastRenderedPageBreak/></w:r></w:p>'
           '<w:p><w:r><w:t>次</w:t></w:r></w:p>' + office.DOCX_SECT + '</w:body></w:document>')
    docx = office.make_docx(d / 'controls.docx', doc)
    (d / 'k5.keys').write_text('SNAP RET SNAP "F" RET C-s SNAP\n')
    _, s = h.open(docx, d / 'k5.keys', d / 'c')
    require(screen(s[0])[:4] == ['A   B', 'C', '', 'D-E'] and separators(s[0]) == [3], screen(s[0])[:4])
    require(row_text(s[1], 23) == '  段落: A→B↓C≡D-E', row_text(s[1], 23))
    require(s[2]['notice'].startswith('保存しました'), s[2]['notice'])
    with zipfile.ZipFile(docx) as z:
        new = z.read('word/document.xml')
    require(('<w:p><w:r><w:rPr><w:sz w:val="28"/></w:rPr><w:t xml:space="preserve">A</w:t><w:tab/><w:t xml:space="preserve">B</w:t><w:br/>'
             '<w:t xml:space="preserve">C</w:t><w:br w:type="page"/><w:t xml:space="preserve">D</w:t><w:noBreakHyphen/>'
             '<w:t xml:space="preserve">EF</w:t></w:r></w:p>').encode() in new, new)
    # Nothing changed: C-s says so and writes nothing.
    before = docx.read_bytes()
    (d / 'k3.keys').write_text('C-s SNAP\n')
    _, s = h.open(docx, d / 'k3.keys', d / 'r')
    require(s[0]['notice'] == '変更はありません' and docx.read_bytes() == before, 'no changes, nothing written')
    # A big document: 3000 paragraphs and 200 tables open and save.
    body = []
    for i in range(3000):
        body.append(('p', office.w_para(office.w_run(f'段落 {i} ' + 'あいう' * (i % 40)))))
        if i % 15 == 0:
            body.append(('t', office.docx_table()))
    docx = office.make_docx(d / 'big.docx', office.docx_document(body))
    (d / 'k4.keys').write_text('END RET "末" RET PGUP PGUP SNAP C-s SNAP\n')
    _, s = h.open(docx, d / 'k4.keys', d / 'b')
    require(s[1]['notice'].startswith('保存しました'), 'big document saved')
    with zipfile.ZipFile(docx) as z:
        c, _, _ = body_children(z.read('word/document.xml'))
    require(len(c) == len(body) + 1 and para_text(element(c[-2][1])).endswith('末'), 'big document edited at the end')


def dirty(snap):
    return row_text(snap, 0).endswith('変更あり')


def check_undo(h, out):
    """Revision 1 item 38: C-z / C-y, one paragraph's change a step; the
    「変更あり」 mark against the saved state; UNDO_LEVELS."""
    d = out / 'undo'
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    body = [('p', office.w_para(office.w_run('一つ目'))),
            ('p', office.w_para(office.w_run('太字', '<w:b/>') + office.w_run('普通'), attrs=' w:rsidR="00A10001"')),
            ('p', office.w_para(office.w_run('三つ目'))), ('p', office.w_para(office.w_run('四つ目')))]
    docx = office.make_docx(d / 'undo.docx', office.docx_document(body))
    shutil.copy(docx, d / 'original.docx')
    with zipfile.ZipFile(docx) as z:
        original = z.read('word/document.xml')
    stdout, s = h.open(docx, ROOT / 'tests/doc_undo.keys', d / 'u')
    require(len(s) == 19 and stdout.strip().endswith('END 0'), f'{len(s)} snapshots\n{stdout}')
    notices = [x['notice'] for x in s]
    four = ['一つ目', '太字普通', '三つ目', '四つ目']
    require(screen(s[0])[:4] == four and not dirty(s[0]), screen(s[0])[:4])
    require(notices[1] == '取り消す物はありません' and s[1]['widget'] == 1, (notices[1], s[1]['widget']))   # a notice, not an error box
    require(screen(s[2])[1] == '二を直した' and dirty(s[2]), 'edited')
    require(notices[3] == '取り消しました（2 段落目）' and screen(s[3])[:4] == four and not dirty(s[3]) and status(s[3]) == '行 2/4',
            (notices[3], screen(s[3])[:4], status(s[3])))
    require(notices[4] == 'やり直しました（2 段落目）' and screen(s[4])[1] == '二を直した' and dirty(s[4]), notices[4])
    require(notices[5] == 'やり直す物はありません', notices[5])
    require(screen(s[6])[:5] == ['一つ目', '二を直した', '三つ目', '足した', '四つ目'], screen(s[6])[:5])
    require(screen(s[7])[:4] == ['二を直した', '三つ目', '足した', '四つ目'], screen(s[7])[:4])
    require(notices[8] == '取り消しました（1 段落目）' and screen(s[8])[0] == '一つ目' and status(s[8]) == '行 1/5', (notices[8], status(s[8])))
    require(notices[9] == '取り消しました（4 段落目）' and screen(s[9])[:4] == ['一つ目', '二を直した', '三つ目', '四つ目']
            and status(s[9]) == '行 3/4', (notices[9], status(s[9])))
    require(notices[10] == '保存しました（元のファイルは .bak に残しました）' and not dirty(s[10]), notices[10])
    require(notices[11] == '取り消しました（2 段落目）' and screen(s[11])[1] == '太字普通' and dirty(s[11]), 'undone after saving')
    require(notices[12] == '保存しました' and not dirty(s[12]), notices[12])
    with zipfile.ZipFile(docx) as z:
        require(z.read('word/document.xml') == original, 'undone and saved: the main part is the original, byte for byte')
    require(Path(str(docx) + '.bak').read_bytes() == (d / 'original.docx').read_bytes(), '.bak is the original')
    require(notices[13] == 'やり直しました（2 段落目）' and dirty(s[13]), 'redone after saving')
    require(notices[14] == '取り消しました（2 段落目）' and not dirty(s[14]), 'back to the saved state clears the mark')
    require(screen(s[15])[2] == '三を直した' and dirty(s[15]), 'new edit')
    require(notices[16] == 'やり直す物はありません', 'a new edit drops what could be redone')
    require(notices[17] == '取り消しました（3 段落目）' and not dirty(s[17]) and screen(s[17])[:4] == four, notices[17])
    require(s[18]['depth'] == 0, 'not changed: Esc closes without asking')
    with zipfile.ZipFile(docx) as z, zipfile.ZipFile(d / 'original.docx') as o:
        require(z.namelist() == o.namelist() and z.testzip() is None, 'entries')
        for name in o.namelist():
            if name != 'word/document.xml':
                require(raw_entry(docx, z.getinfo(name)) == raw_entry(d / 'original.docx', o.getinfo(name)), f'{name} copied byte for byte')
    # UNDO_LEVELS=2: three edits, two steps kept.
    docx = office.make_docx(d / 'levels.docx', office.docx_document(body))
    stdout, s = h.open(docx, ROOT / 'tests/doc_undo_levels.keys', d / 'l', levels=2)
    notices = [x['notice'] for x in s]
    require(screen(s[1])[:3] == ['A', 'B', 'C'], screen(s[1])[:3])
    require(screen(s[2])[:3] == ['A', '太字普通', '三つ目'] and notices[2] == '取り消しました（2 段落目）' and dirty(s[2]), (screen(s[2])[:3], notices[2]))
    require(notices[3] == '取り消す物はありません' and screen(s[3])[0] == 'A' and dirty(s[3]), 'the oldest step is gone; still changed')
    require(screen(s[4])[:3] == ['A', 'B', 'C'] and notices[4] == 'やり直す物はありません', (screen(s[4])[:3], notices[4]))
    asked = ''.join(row_text(s[5], y).strip() for y in range(1, 23))
    require(s[5]['widget'] == 2 and '保存せずに戻りますか？' in asked and s[6]['depth'] == 0, 'asks, leaves')
    # The default, 100: 101 edits, the 101st undo finds nothing.
    docx = office.make_docx(d / 'hundred.docx', office.docx_document(body))
    keys = ''.join(f'RET C-u "x{i}" RET\n' for i in range(101)) + 'SNAP\n' + 'C-z\n' * 100 + 'SNAP C-z SNAP\n'
    (d / 'hundred.keys').write_text(keys)
    _, s = h.open(docx, d / 'hundred.keys', d / 'h')
    require(screen(s[0])[0] == 'x100' and screen(s[1])[0] == 'x0' and dirty(s[1]), (screen(s[0])[0], screen(s[1])[0]))
    require(s[1]['notice'] == '取り消しました（1 段落目）' and s[2]['notice'] == '取り消す物はありません' and screen(s[2])[0] == 'x0',
            'UNDO_LEVELS 100 by default')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    build_dir = args.build.resolve()
    out = build_dir / ('doc-check-asan' if args.sanitize else 'doc-check')
    out.mkdir(parents=True, exist_ok=True)
    h = Harness(build(build_dir, args.sanitize))
    check_view(h, out)
    check_edit(h, out)
    check_broken(h, out)
    check_strange(h, out)
    check_undo(h, out)
    print(f'stage 5 doc checks passed: {checks} checks')


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError, subprocess.CalledProcessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
