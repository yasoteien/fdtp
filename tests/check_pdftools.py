#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Revision 1 item 39 (the PDF menu) acceptance test, run from the Mac.

    check_pdftools.py [--deploy] [--fdtp] [--only CASE...]

--deploy  sends the working tree to the build host's ~/work/fdtp-src-w3,
          cross builds tests/pdftools_harness.c with src/pdftools.c,
          src/pdftools_job.c, src/pdfview.c and libmupdf (-Werror), and puts
          it and the libraries the Pomera lacks in the Pomera's
          ~/work/fdtp-pdftools/{bin,lib}.
--fdtp    also cross builds fdtp itself there (make, the warnings counted)
          and puts it in ~/work/fdtp-pdftools/bin (never ~/.fdtp).
The ssh names are FDTP_BUILD_HOST and FDTP_POMERA_HOST (tests/dev_env.py).
The test PDFs and images are made on the Pomera by this script itself
(--fixtures DIR, standard library only, with tools/make_test_pdf.py), each
case runs in its own copy under ~/work/fdtp-pdftools/check, with flock on
/tmp/pomera-heavy.lock (the only thing in /tmp). The outputs are opened
again with the harness's inspect (MuPDF) and their JPEG headers read.
"""
import argparse
import importlib.util
import io
import json
import os
import random
import re
import struct
import subprocess
import sys
import tarfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
BUILD_DIR, POMERA_DIR = 'work/fdtp-src-w3', 'work/fdtp-pdftools'
RUNTIME_LIBS = ['libmupdf.so.25.1', 'libjbig2dec.so.0', 'libgumbo.so.3', 'libmujs.so.3']
# Taken in turn with the other workers' tests; 75: still busy after 60 s.
LOCK = 'flock -o -w 60 -E 75 /tmp/pomera-heavy.lock'
LOCK_BUSY = 75

# ---- fixtures (also run on the Pomera: --fixtures) ---------------------------

# pages10.pdf: page i (0-based) is (210 + 10 i) x 300 points, so the order of
# the pages of every output can be read from the widths.
PAGES10 = [(210 + 10 * i, 300) for i in range(10)]
SAMPLE = [(595, 842), (842, 595), (300, 200)]   # tools/make_test_pdf.py PAGES


def simple_pdf(pages, image=None, own=False):
    """pages: [(w, h)]; image: (width, height, rgb bytes, drawn size in points)
    on every page (Flate), or None; own: every page its own image (the
    bytes changed by the page's number)."""
    objs = []
    def add(body):
        objs.append(body); return len(objs)
    font = add(b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>')
    def add_image(data):
        w, h = image[0], image[1]
        z = zlib.compress(data, 1)
        return add(b'<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB /BitsPerComponent 8 '
                   b'/Filter /FlateDecode /Length %d >>\nstream\n' % (w, h, len(z)) + z + b'\nendstream')
    img = add_image(image[2]) if image else None
    parent = len(objs) + 1; objs.append(None)
    kids = []
    for i, (w, h) in enumerate(pages):
        if image and own and i:
            img = add_image(bytes([i & 255]) + image[2][1:])
        ops = b'BT /F1 18 Tf 20 %d Td (Page %d) Tj ET' % (h - 40, i + 1)
        if img:
            ops += b' q %d 0 0 %d 20 20 cm /Im0 Do Q' % (image[3], image[3])
        c = add(b'<< /Length %d >>\nstream\n' % len(ops) + ops + b'\nendstream')
        res = b'/Font << /F1 %d 0 R >>' % font + (b' /XObject << /Im0 %d 0 R >>' % img if img else b'')
        kids.append(add(b'<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %d %d] /Contents %d 0 R /Resources << %s >> >>' % (parent, w, h, c, res)))
    objs[parent - 1] = b'<< /Type /Pages /Kids [%s] /Count %d >>' % (b' '.join(b'%d 0 R' % k for k in kids), len(kids))
    root = add(b'<< /Type /Catalog /Pages %d 0 R >>' % parent)
    out = bytearray(b'%PDF-1.4\n'); offsets = []
    for n, body in enumerate(objs, 1):
        offsets.append(len(out)); out += b'%d 0 obj\n' % n + body + b'\nendobj\n'
    xref = len(out)
    out += b'xref\n0 %d\n0000000000 65535 f \n' % (len(objs) + 1)
    for o in offsets: out += b'%010d 00000 n \n' % o
    out += b'trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (len(objs) + 1, root, xref)
    return bytes(out)


def noise(w, h, seed):
    r = random.Random(seed)
    return r.randbytes(w * h * 3)


def png(w, h, dpi=None):
    rows = b''.join(b'\0' + bytes(v for x in range(w) for v in ((x * 4) & 255, (y * 5) & 255, 128)) for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    body = chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    if dpi: body += chunk(b'pHYs', struct.pack('>IIB', round(dpi / 0.0254), round(dpi / 0.0254), 1))
    return b'\x89PNG\r\n\x1a\n' + body + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')


def make_fixtures(out):
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('make_test_pdf', Path(__file__).resolve().parent / 'make_test_pdf.py')
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    m.generate(out)                                  # sample, password, broken, truncated
    (out / 'pages10.pdf').write_bytes(simple_pdf(PAGES10))
    # A 1000 x 1000 photo-like image (noise) drawn over 72 points: 1000 dpi.
    (out / 'photo.pdf').write_bytes(simple_pdf([(200, 200)], (1000, 1000, noise(1000, 1000, 1), 72)))
    # 100 pages, each with its own 256 x 256 image over 72 points (256 dpi).
    (out / 'big100.pdf').write_bytes(simple_pdf([(595, 842)] * 100, (256, 256, noise(256, 256, 2), 72), own=True))
    (out / 'a.png').write_bytes(png(64, 48, 150))
    (out / 'b.png').write_bytes(png(40, 80))
    (out / 'note.txt').write_text('not a PDF\n')
    # sample.pdf with a wrong startxref: MuPDF repairs it by reading the
    # objects (written last: its presence says the fixtures are complete).
    sample = (out / 'sample.pdf').read_bytes()
    (out / 'badxref.pdf').write_bytes(re.sub(rb'startxref\n\d+', b'startxref\n17', sample))


def remote_list(directory):
    """Files of directory: name, size, and a JPEG's width x height."""
    result = []
    for p in sorted(Path(directory).iterdir()):
        if not p.is_file(): continue
        item = {'name': p.name, 'size': p.stat().st_size}
        data = p.read_bytes()[:65536]
        if data[:2] == b'\xff\xd8':
            i = 2
            while i + 9 < len(data):
                if data[i] != 0xff: break
                marker, length = data[i + 1], struct.unpack('>H', data[i + 2:i + 4])[0]
                if marker in (0xc0, 0xc1, 0xc2):
                    item['jpeg'] = list(struct.unpack('>HH', data[i + 5:i + 9]))[::-1]; break
                i += 2 + length
        result.append(item)
    print(json.dumps(result))


# ---- the Mac side ------------------------------------------------------------

sys.path.insert(0, str(Path(__file__).resolve().parent))


def hosts():
    import dev_env
    return dev_env.get('FDTP_BUILD_HOST', 'build-host'), dev_env.get('FDTP_POMERA_HOST', 'pomera')


def sh(host, command, data=None, check=True):
    r = subprocess.run(['ssh', host, command], input=data, capture_output=True)
    if check and r.returncode:
        sys.exit('%s: %s\n%s%s' % (host, command[:300], r.stdout.decode(errors='replace')[-3000:], r.stderr.decode(errors='replace')[-3000:]))
    return r.stdout


def deploy(build_fdtp):
    build, pomera = hosts()
    files = subprocess.run(['git', '-C', str(REPO), 'ls-files', '-z', '--cached', '--others', '--exclude-standard', 'fdtp', 'port/app/platform'],
                           capture_output=True, check=True).stdout.split(b'\0')
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w:gz') as tar:
        for f in files:
            p = REPO / f.decode()
            if f and p.is_file(): tar.add(p, arcname=f.decode())
    sh(build, 'mkdir -p ~/{d} && tar -C ~/{d} -m -xzf - && mkdir -p ~/{d}/linklib && cp -n ~/work/fdtp-pdf/linklib/* ~/{d}/linklib/'.format(d=BUILD_DIR), buf.getvalue())
    sysroot = '$HOME/work/sysroot-armhf'; lib = sysroot + '/usr/lib/arm-linux-gnueabihf'
    out = '$HOME/%s/fdtp/build-arm-linux-gnueabihf' % BUILD_DIR
    # pdfview's note input (item 40) needs doc_input and an input method.
    support = ' '.join('src/%s.c' % m for m in 'pdftools pdftools_job order_box pdfview app widgets keys commands canvas cells raster font24 doc_input'.split()) + ' tests/ime_fake.c'
    log = sh(build, 'cd ~/{d}/fdtp && mkdir -p {o}/tmp && export TMPDIR={o}/tmp && (test -f {o}/font24.inc || python3 tools/gen_font.py --output {o}/font24.inc) && '
             'arm-linux-gnueabihf-gcc -O2 -std=c11 -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 -Wall -Wextra -Werror -DFDTP_WITH_MUPDF -Isrc -I{o} -I{s}/usr/include -I{s}/usr/include/arm-linux-gnueabihf '
             '{src} tests/pdftools_harness.c -o {o}/pdftools-harness -L{l} -Wl,-rpath-link,{l} -Wl,-rpath-link,$HOME/{d}/linklib '
             "-Wl,--disable-new-dtags -Wl,-rpath,'$ORIGIN/../lib' -lmupdf -lm 2>&1".format(d=BUILD_DIR, o=out, s=sysroot, l=lib, src=support))
    print('harness built%s' % (': ' + log.decode() if log.strip() else ' (no warnings)'))
    # build host -> Mac -> Pomera with rsync at 200 KB/s (a fast transfer
    # froze the Pomera's Wi-Fi: docs/FDTP_04 §5, deploy.sh).
    dist = ROOT / 'build/pdftools-dist/bin'; dist.mkdir(parents=True, exist_ok=True)
    (dist / 'pdftools-harness').write_bytes(sh(build, 'cat %s/pdftools-harness' % out)); (dist / 'pdftools-harness').chmod(0o755)
    if build_fdtp:
        # fdtp itself, for the real screen.
        sh(build, 'cd ~/{d}/fdtp && make -j6 CROSS=arm-linux-gnueabihf- all > {o}/../cross-build-w3.log 2>&1 || (tail -30 {o}/../cross-build-w3.log; exit 1)'.format(d=BUILD_DIR, o=out))
        warnings = sh(build, "grep -c 'warning:' ~/%s/fdtp/cross-build-w3.log || true" % BUILD_DIR).decode().strip()
        print('fdtp cross build: %s warnings' % warnings)
        (dist / 'fdtp').write_bytes(sh(build, 'cat %s/fdtp' % out)); (dist / 'fdtp').chmod(0o755)


def send_scripts():
    """This script and the tools it uses into fixtures/ (rsync, 200 KB/s)."""
    _, pomera = hosts()
    dist = ROOT / 'build/pdftools-dist'; (dist / 'fixtures').mkdir(parents=True, exist_ok=True)
    for src in (Path(__file__), ROOT / 'tools/make_test_pdf.py', ROOT / 'tools/ppm2png.py'):
        (dist / 'fixtures' / src.name).write_bytes(src.read_bytes())
    subprocess.run(['rsync', '-rltp', '-c', '--partial', '--bwlimit=200', str(dist) + '/', '%s:%s/' % (pomera, POMERA_DIR)], check=True)


def parse(stdout):
    """The harness's lines: snaps (n -> dict), and the rest."""
    snaps, info, order, history = {}, {}, [], []
    cur = None
    for line in stdout.decode(errors='replace').splitlines():
        head, _, rest = line.partition(' ')
        if head == 'SNAP':
            n, depth, widget = map(int, rest.split()); cur = snaps[n] = {'depth': depth, 'widget': widget, 'rows': [], 'boxes': [],
                                                                          'label': '', 'text': None, 'info': [], 'glyphs': []}
        elif head == 'ROW' and cur is not None: cur['rows'].append(rest.partition(' ')[2])
        elif head == 'BOX' and cur is not None: cur['boxes'].append(list(map(int, rest.split())))
        elif head == 'LABEL' and cur is not None: cur['label'] = rest
        elif head == 'TEXT' and cur is not None: cur['text'] = rest
        elif head == 'INFO' and cur is not None: cur['info'].append(tuple(rest.split('|', 1)))
        elif head == 'GLYPH' and cur is not None: cur['glyphs'].append(rest)
        elif head == 'ORDER': order.append(rest)
        elif head == 'HISTORY': history.append(rest)
        else: info[head] = rest
    return snaps, info, order, history


class Pomera:
    """The cases run on the Pomera itself (--local, started by one ssh from
    the Mac), so no ssh is opened per step."""
    def __init__(self):
        self.base = '~/%s' % POMERA_DIR

    def sh(self, command, check=True):
        r = subprocess.run(['bash', '-c', command], capture_output=True)
        if check and r.returncode:
            sys.exit('%s\n%s%s' % (command[:300], r.stdout.decode(errors='replace')[-3000:], r.stderr.decode(errors='replace')[-3000:]))
        return r.stdout

    def run(self, case, files, entry, page, size, names, keys, env=''):
        """Runs the harness in check/CASE (a fresh copy of files)."""
        d = '%s/check/%s' % (self.base, case)
        script = ' '.join(keys.split())
        cmd = ('rm -rf {d} && mkdir -p {d} && cd {b}/fixtures && cp {f} {d}/ && cd {d} && printf %s "$K" > ../{c}.keys && '
               '{env} {lock} env FDTP_LANGUAGE=ja {b}/bin/pdftools-harness run {d} ../{c}.keys ../{c} {e} {p} {s} {n}; echo RC $?').format(
            d=d, b=self.base, f=' '.join(files), c=case, env=env, lock=LOCK, e=entry, p=page, s=size, n=' '.join(names))
        return self.parsed('K=%s; %s' % (shquote(script), cmd))

    def parsed(self, command):
        """Runs a harness command (with LOCK), up to 5 times while the lock is busy."""
        for _ in range(5):
            result = parse(self.sh(command))
            if result[1].get('RC') != str(LOCK_BUSY): return result
            print('(the lock is busy; waiting again)', flush=True)
        sys.exit('the lock /tmp/pomera-heavy.lock stayed busy')

    def inspect(self, case, name, password=None):
        out = self.sh('cd %s/check/%s && env FDTP_LANGUAGE=ja %s/bin/pdftools-harness inspect %s %s' % (self.base, case, self.base, shquote(name), shquote(password) if password else '')).decode()
        result = {'pages': []}
        for line in out.splitlines():
            w = line.split()
            if w[0] == 'OPEN': result['open'] = w[1]
            elif w[0] == 'ENCRYPTED': result['encrypted'] = int(w[1]); result['method'] = w[3]; result['bits'] = int(w[5])
            elif w[0] == 'REPAIRED': result['repaired'] = int(w[1])
            elif w[0] == 'PAGES': result['count'] = int(w[1])
            elif w[0] == 'PAGE': result['pages'].append({'w': float(w[2]), 'h': float(w[3]), 'rotate': int(w[5]), 'images': int(w[7]), 'imgw': int(w[9]), 'imgh': int(w[11])})
        return result

    def files(self, case):
        out = self.sh('cd %s/fixtures && python3 -I check_pdftools.py --list %s/check/%s' % (self.base, self.base, case))
        return {f['name']: f for f in json.loads(out)}

    def hidden(self):
        return self.sh("find %s/check -name '.*.fdtp-*' | head" % self.base).decode().split()


def shquote(s):
    return "'" + s.replace("'", "'\\''") + "'"


checks = 0


def check(cond, what):
    global checks
    if not cond:
        sys.exit('FAILED: %s' % (what,))
    checks += 1


def screen_ok(snaps, case, cols):
    """Every snapshot: characters in the font; floating boxes on the screen
    (their shadows too) between row 1 and the message line."""
    rows = 25
    for n, s in snaps.items():
        check(not s['glyphs'], (case, n, 'glyphs', s['glyphs']))
        for col, row, w, h, kind, rule in s['boxes'][1:]:
            check(col >= 0 and col + w + (1 if kind & 2 else 0) <= cols and row >= 1 and row + h <= rows - 2,
                  (case, n, 'box off the screen', col, row, w, h))


def text(s):
    return '\n'.join(s['rows'])


def widths(r):
    return [round(p['w']) for p in r['pages']]


CASES = {}


def case(fn):
    CASES[fn.__name__] = fn; return fn


@case
def menu(p):
    for size, cols in (('85x25', 85), ('80x25', 80)):
        snaps, info, _, _ = p.run('menu' + size, ['sample.pdf'], 'menu', -1, size, ['sample.pdf'],
                                  'SNAP DOWN DOWN SNAP END SNAP HOME SNAP m SNAP ESC SNAP ESC SNAP')
        screen_ok(snaps, 'menu', cols)
        s = snaps[1]; t = text(s)
        check(info['OPENED'] == '0' and s['depth'] == 2, ('menu open', info))
        for name in ('PDF のメニュー', '対象 sample.pdf', '結合', '分割', 'ページの抽出', 'ページの削除', '並べ替え', '回転', '保護',
                     'ロック解除', '圧縮', 'PDF→JPEG', '画像→PDF', '修復', '↑↓ 選ぶ'):
            check(name in t, ('menu item', size, name))
        # The box: 12 items + the line between the groups, title, target, rule.
        box = s['boxes'][1]
        check(box[3] == 17 and box[5] == box[1] + 2, ('menu box', box))
        row = lambda s, word: next(i for i, r in enumerate(s['rows']) if word in r)
        check(row(s, '分割') == box[1] + 4, 'first enabled entry (split) chosen')
        check(row(s, '圧縮') == row(s, 'ロック解除') + 2, 'a line between the two groups')
        check('結合する PDF を 2 つ以上' in snaps[5]['label'] and snaps[5]['widget'] == 6, ('merge refused', snaps[5]['label']))
        check(snaps[6]['depth'] == 2 and 'PDF のメニュー' in text(snaps[6]), 'error closed, menu again')
        check(snaps[7]['depth'] == 1, 'Esc leaves the menu')
    # Images marked: only 画像→PDF; a text file: refused at once.
    snaps, info, _, _ = p.run('menu_images', ['a.png', 'b.png'], 'menu', -1, '80x25', ['a.png', 'b.png'], 'SNAP s SNAP')
    check('対象 画像 2 枚' in text(snaps[1]), 'images target')
    check('PDF を 1 つだけ' in snaps[2]['label'], snaps[2]['label'])
    snaps, info, _, _ = p.run('menu_text', ['note.txt'], 'menu', -1, '80x25', ['note.txt'], 'SNAP')
    check(info['OPENED'] == '-1' and snaps[1]['depth'] == 1 and 'PDF を選んでください' in snaps[1]['label'], ('text refused', info, snaps[1]['label']))
    snaps, info, _, _ = p.run('merge_one', ['sample.pdf'], 'merge', -1, '80x25', ['sample.pdf'], 'SNAP')
    check(info['OPENED'] == '-1' and '2 つ以上' in snaps[1]['label'], 'Alt+h with one PDF')
    snaps, info, _, _ = p.run('merge_mixed', ['sample.pdf', 'pages10.pdf', 'note.txt'], 'merge', -1, '80x25', ['sample.pdf', 'pages10.pdf', 'note.txt'], 'SNAP')
    check(info['OPENED'] == '-1' and 'PDF 以外' in snaps[1]['label'], 'Alt+h with a text file')


@case
def viewer(p):
    # Alt+p in the viewer (W4 calls pdftools_open with its page): over the
    # picture, the page as the field's first value.
    for size, cols in (('85x25', 85), ('80x25', 80)):
        snaps, info, _, _ = p.run('viewer' + size, ['pages10.pdf'], 'menu', 2, size, ['pages10.pdf'],
                                  'SNAP e SNAP RET SNAP RET SNAP RET WAIT:5 SNAP ESC SNAP')
        screen_ok(snaps, 'viewer', cols)
        check('今のページ 3' in text(snaps[1]) and snaps[1]['depth'] == 3, ('viewer page', text(snaps[1])[:300]))
        check(snaps[2]['text'] == '3', ('extract initial', snaps[2]['text']))
        check('書き出し先' in snaps[3]['label'], snaps[3]['label'])
        check(snaps[4]['text'] == 'pages10_extract.pdf', snaps[4]['text'])
        check(snaps[5]['widget'] == 5 and any('pages10_extract.pdf  1 ページ' in v for _, v in snaps[5]['info']), snaps[5]['info'])
        check(snaps[6]['depth'] == 2, 'back in the viewer')
        r = p.inspect('viewer' + size, 'pages10_extract.pdf')
        check(r['count'] == 1 and widths(r) == [230], r)
        # The menu box is on the canvas: its frame's cyan through the box's top row.
        ppm = p.ppm('viewer' + size, 1)
        box = snaps[1]['boxes'][-1]
        y = box[1] * 24 + 12
        x0 = (1024 - cols * 12) // 2 + (box[0] + box[2] // 2) * 12
        check(any(ppm(x0 + dx, y + dy)[2] > 200 and ppm(x0 + dx, y + dy)[0] < 100 for dx in range(12) for dy in range(-3, 4)), 'menu frame drawn over the page')


@case
def split(p):
    snaps, info, _, _ = p.run('split', ['sample.pdf'], 'split', -1, '85x25', ['sample.pdf'],
                              'SNAP r SNAP "1,2-" RET WAIT:5 SNAP')
    screen_ok(snaps, 'split', 85)
    check('分割のしかた' in snaps[1]['label'], snaps[1]['label'])
    check(snaps[2]['text'] == '' and '全 3 ページ' in snaps[2]['label'], (snaps[2]['label'], snaps[2]['text']))
    inf = dict(snaps[3]['info'])
    check(snaps[3]['label'] == '分割しました' and '1 ページ' in inf['1'] and '2 ページ' in inf['2'] and '3 ページ' in inf['合計'], snaps[3]['info'])
    check(int(info['RELOADS']) >= 1, ('filer reloaded', info['RELOADS']))
    a, b = p.inspect('split', 'sample_1.pdf'), p.inspect('split', 'sample_2-3.pdf')
    check(widths(a) == [595] and widths(b) == [842, 300], (widths(a), widths(b)))
    # N pages each.
    snaps, info, _, _ = p.run('split_every', ['pages10.pdf'], 'menu', -1, '80x25', ['pages10.pdf'], 's n SNAP C-u "3" RET WAIT:5 SNAP')
    check(snaps[1]['text'] == '1' and '何ページごと' in snaps[1]['label'], snaps[1]['label'])
    names = {'pages10_1-3.pdf': [210, 220, 230], 'pages10_4-6.pdf': [240, 250, 260], 'pages10_7-9.pdf': [270, 280, 290], 'pages10_10.pdf': [300]}
    for name, w in names.items():
        check(widths(p.inspect('split_every', name)) == w, (name, w))
    # Errors: out of range, empty, wrong, the same name twice, every N >= pages; Esc leaves nothing.
    snaps, info, _, _ = p.run('split_bad', ['sample.pdf'], 'split', -1, '80x25', ['sample.pdf'],
                              'r "5" RET SNAP ESC WAIT:0 SNAP C-u RET SNAP ESC WAIT:0 C-u "1-2,x" RET SNAP ESC WAIT:0 '
                              'C-u "0" RET SNAP ESC WAIT:0 C-u "1-2,1-2" RET SNAP ESC WAIT:0 SNAP ESC SNAP')
    check('ページの範囲が違います（1〜3）' in snaps[1]['label'], snaps[1]['label'])
    check(snaps[2]['text'] == '5', ('asked again with the text', snaps[2]['text']))
    check('ページの範囲が空です' in snaps[3]['label'], snaps[3]['label'])
    check('正しくありません' in snaps[4]['label'], snaps[4]['label'])
    check('違います' in snaps[5]['label'], snaps[5]['label'])
    check('同じ名前のファイルが 2 つ' in snaps[6]['label'], snaps[6]['label'])
    check(snaps[7]['widget'] == 3 and snaps[8]['depth'] == 1, 'Esc gives up')
    check(set(p.files('split_bad')) == {'sample.pdf'}, p.files('split_bad'))
    snaps, info, _, _ = p.run('split_every_bad', ['sample.pdf'], 'split', -1, '80x25', ['sample.pdf'], 'n C-u "3" RET SNAP')
    check('ページの数が違います（1〜2）' in snaps[1]['label'], snaps[1]['label'])
    snaps, info, _, _ = p.run('split_one', ['password.pdf'], 'split', -1, '80x25', ['password.pdf'], 'SNAP "secret" RET SNAP')
    check('1 ページの PDF は分けられません' in snaps[2]['label'], snaps[2]['label'])
    # Overwrite: No keeps the files, Yes writes them again.
    p.run('split_again', ['sample.pdf'], 'split', -1, '80x25', ['sample.pdf'], 'r "1,2-" RET WAIT:5 SNAP')
    before = p.files('split_again')
    p.sh('sleep 1.1')
    snaps, info, _, _ = rerun(p, 'split_again', 'split', ['sample.pdf'], 'r "1,2-" RET SNAP n SNAP')
    check('2 個のファイルが既にあります' in snaps[1]['label'], snaps[1]['label'])
    check(p.files('split_again') == before and snaps[2]['depth'] == 1, 'No: nothing written')
    snaps, info, _, _ = rerun(p, 'split_again', 'split', ['sample.pdf'], 'r "1,2-" RET y WAIT:5 SNAP')
    check(snaps[1]['label'] == '分割しました' and set(p.files('split_again')) == set(before), snaps[1]['label'])


def rerun(p, case, entry, names, keys, page=-1, size='80x25', env=''):
    """The harness again in check/CASE as it is (no fresh copy)."""
    d = '%s/check/%s' % (p.base, case)
    cmd = ('cd {d} && printf %s "$K" > ../{c}-again.keys && {env} {lock} env FDTP_LANGUAGE=ja {b}/bin/pdftools-harness run {d} ../{c}-again.keys ../{c}-again {e} {p} {s} {n}; echo RC $?'
           ).format(d=d, b=p.base, c=case, env=env, lock=LOCK, e=entry, p=page, s=size, n=' '.join(names))
    return p.parsed('K=%s; %s' % (shquote(' '.join(keys.split())), cmd))


@case
def pages(p):
    snaps, info, _, _ = p.run('extract', ['pages10.pdf'], 'menu', -1, '80x25', ['pages10.pdf'], 'e SNAP "3,1,5-6" RET n SNAP RET WAIT:5 SNAP')
    check(snaps[1]['text'] == '' and '抽出するページ（全 10 ページ' in snaps[1]['label'], snaps[1]['label'])
    check(snaps[2]['text'] == 'pages10_extract.pdf', snaps[2]['text'])
    check(widths(p.inspect('extract', 'pages10_extract.pdf')) == [230, 210, 250, 260], 'extract order')
    rerun(p, 'extract', 'menu', ['pages10.pdf'], 'e "3-1" RET n C-u "back.pdf" RET WAIT:5 SNAP')
    check(widths(p.inspect('extract', 'back.pdf')) == [230, 220, 210], 'a range backwards')
    snaps, info, _, _ = p.run('extract_twice', ['pages10.pdf'], 'menu', -1, '80x25', ['pages10.pdf'], 'e "1,1" RET SNAP')
    check('同じページが 2 回以上' in snaps[1]['label'], snaps[1]['label'])
    snaps, info, _, _ = p.run('delete', ['pages10.pdf'], 'menu', -1, '80x25', ['pages10.pdf'], 'd "2-9" RET n RET WAIT:5 SNAP')
    check(widths(p.inspect('delete', 'pages10_deleted.pdf')) == [210, 300], 'delete')
    snaps, info, _, _ = p.run('delete_all', ['pages10.pdf'], 'menu', -1, '80x25', ['pages10.pdf'], 'd "1-" RET SNAP ESC WAIT:0 SNAP')
    check('全部のページは削除できません' in snaps[1]['label'] and snaps[2]['text'] == '1-', snaps[1]['label'])
    # Reorder: the default is the order as it is (refused); "10,1": the rest after; replacing the original.
    snaps, info, _, _ = p.run('reorder', ['pages10.pdf'], 'menu', -1, '80x25', ['pages10.pdf'],
                              'o SNAP RET SNAP ESC WAIT:0 C-u "10,1" RET r SNAP y WAIT:5 SNAP')
    check(snaps[1]['text'] == '1-10', snaps[1]['text'])
    check('順番が変わりません' in snaps[2]['label'], snaps[2]['label'])
    check('元のファイル pages10.pdf を置き換えますか' in snaps[3]['label'], snaps[3]['label'])
    r = p.inspect('reorder', 'pages10.pdf')
    check(widths(r) == [300, 210, 220, 230, 240, 250, 260, 270, 280, 290], widths(r))
    check(set(p.files('reorder')) == {'pages10.pdf'}, p.files('reorder'))
    snaps, info, _, _ = p.run('rotate', ['sample.pdf'], 'menu', -1, '80x25', ['sample.pdf'], 'r SNAP C-u "1,3,3" RET SNAP r n RET WAIT:5 SNAP')
    check(snaps[1]['text'] == '1-3', snaps[1]['text'])
    check('回す向き' in snaps[2]['label'], snaps[2]['label'])
    r = p.inspect('rotate', 'sample_rotated.pdf')
    check([x['rotate'] for x in r['pages']] == [90, 0, 90], r)
    snaps, info, _, _ = rerun(p, 'rotate', 'menu', ['sample_rotated.pdf'], 'r C-u "1-2" RET l r y WAIT:5 SNAP')
    r = p.inspect('rotate', 'sample_rotated.pdf')
    check([x['rotate'] for x in r['pages']] == [0, 270, 90], r)


@case
def passwords(p):
    snaps, info, _, history = p.run('protect', ['sample.pdf'], 'menu', -1, '85x25', ['sample.pdf'],
                                    'p SNAP "pass word!" SNAP RET SNAP "pass word!" RET n RET WAIT:5 SNAP')
    screen_ok(snaps, 'protect', 85)
    t = text(snaps[2])
    check('付けるパスワード' in t and '**********' in t and 'pass word' not in t, 'stars, not the password')
    check('もう一度' in text(snaps[3]), 'asked twice')
    check(not any('pass' in h for h in history), ('no history of passwords', history))
    check(snaps[4]['label'] == 'パスワードを付けました', snaps[4]['label'])
    check(p.inspect('protect', 'sample_protected.pdf')['open'] == 'password', 'needs the password')
    check(p.inspect('protect', 'sample_protected.pdf', 'wrong')['open'] == 'password', 'a wrong one does not open it')
    r = p.inspect('protect', 'sample_protected.pdf', 'pass word!')
    check(r['open'] == 'ok' and r['encrypted'] == 1 and r['method'] == 'AES' and r['bits'] == 256 and widths(r) == [595, 842, 300], r)
    snaps, info, _, _ = p.run('protect_bad', ['sample.pdf'], 'menu', -1, '80x25', ['sample.pdf'],
                              'p RET SNAP ESC SNAP "abc" RET "abd" RET SNAP ESC SNAP ESC SNAP')
    check('パスワードが空です' in snaps[1]['label'] and '付けるパスワード' in text(snaps[2]), snaps[1]['label'])
    check('一致しません' in snaps[3]['label'] and '付けるパスワード' in text(snaps[4]), snaps[3]['label'])
    check(snaps[5]['depth'] == 1 and set(p.files('protect_bad')) == {'sample.pdf'}, 'Esc in the password box')
    # Unlock: the wrong password is refused and asked again.
    snaps, info, _, _ = p.run('unlock', ['password.pdf'], 'menu', -1, '80x25', ['password.pdf'],
                              'u SNAP "wrong" RET SNAP ESC SNAP "secret" RET n RET WAIT:5 SNAP')
    check('password.pdf のパスワード' in text(snaps[1]), text(snaps[1])[:200])
    check('パスワードが違います' in snaps[2]['label'] and 'password.pdf のパスワード' in text(snaps[3]), snaps[2]['label'])
    r = p.inspect('unlock', 'password_unlocked.pdf')
    check(r['open'] == 'ok' and r['encrypted'] == 0 and r['count'] == 1, r)
    snaps, info, _, _ = p.run('unlock_plain', ['sample.pdf'], 'menu', -1, '80x25', ['sample.pdf'], 'u SNAP')
    check('パスワードが付いていません' in snaps[1]['label'] and snaps[1]['depth'] == 1, snaps[1]['label'])
    # Another entry on a PDF with a password: asked first; the output keeps it.
    snaps, info, _, _ = p.run('locked_extract', ['password.pdf'], 'menu', -1, '80x25', ['password.pdf'], 'e "secret" RET SNAP "1" RET n RET WAIT:5 SNAP')
    check('抽出するページ（全 1 ページ' in snaps[1]['label'], snaps[1]['label'])
    r = p.inspect('locked_extract', 'password_extract.pdf', 'secret')
    check(r['open'] == 'ok' and r['encrypted'] == 1 and r['count'] == 1, r)
    check(p.inspect('locked_extract', 'password_extract.pdf')['open'] == 'password', 'still locked')


@case
def compress(p):
    snaps, info, _, _ = p.run('compress', ['photo.pdf'], 'menu', -1, '80x25', ['photo.pdf'], 'c SNAP RET n RET WAIT:20 SNAP')
    check('どのくらい縮めますか' in snaps[1]['label'], snaps[1]['label'])
    f = p.files('compress'); before, after = f['photo.pdf']['size'], f['photo_compressed.pdf']['size']
    r = p.inspect('compress', 'photo_compressed.pdf')
    # 1000 px over 1 inch, to 150 dpi: MuPDF halves an image's size
    # (a power of two) while it stays at or above that dpi, so 250 px here.
    check(after < before / 4 and r['count'] == 1 and 150 <= r['pages'][0]['imgw'] < 300, (before, after, r))
    check(any('元の大きさ' == n for n, _ in snaps[2]['info']), snaps[2]['info'])
    print('compress: %d -> %d bytes, image %dx%d' % (before, after, r['pages'][0]['imgw'], r['pages'][0]['imgh']))
    # Again on what MuPDF wrote: nothing more to gain, so nothing is written.
    snaps, info, _, _ = rerun(p, 'compress', 'menu', ['photo_compressed.pdf'], 'c RET n RET WAIT:20 SNAP')
    check('小さくなりませんでした' in snaps[1]['label'] and 'photo_compressed_compressed.pdf' not in p.files('compress'), (snaps[1]['label'], p.files('compress')))


@case
def jpeg(p):
    snaps, info, _, _ = p.run('jpeg', ['sample.pdf'], 'menu', -1, '80x25', ['sample.pdf'], 'j SNAP RET SNAP C-u "50" RET SNAP ESC WAIT:0 SNAP C-u "150" RET WAIT:10 SNAP')
    check(snaps[1]['text'] == '1-3', snaps[1]['text'])
    check(snaps[2]['text'] == '150' and 'dpi' in snaps[2]['label'], snaps[2]['label'])
    check('解像度が違います' in snaps[3]['label'] and snaps[4]['text'] == '50', snaps[3]['label'])
    f = p.files('jpeg')
    for i, (w, h) in enumerate(SAMPLE):
        j = f['sample_%d.jpg' % (i + 1)]['jpeg']
        check(abs(j[0] - w * 150 / 72) <= 2 and abs(j[1] - h * 150 / 72) <= 2, (i, j))
    check(snaps[5]['label'] == 'JPEG にしました' and any('3 枚' in v for _, v in snaps[5]['info']), snaps[5]['info'])
    # Their JPEGs back into a PDF (order kept): one page each, the pictures' shapes.
    snaps, info, _, _ = rerun(p, 'jpeg', 'menu', ['sample_1.jpg', 'sample_2.jpg', 'sample_3.jpg'], 'SNAP i SNAP RET SNAP RET WAIT:10 SNAP')
    check('対象 画像 3 枚' in text(snaps[1]) and 'PDF にする画像と順番' in text(snaps[2]) and snaps[3]['text'] == 'sample_1.pdf', snaps[3]['text'])
    r = p.inspect('jpeg', 'sample_1.pdf')
    check(r['count'] == 3 and all(abs(pg['w'] / pg['h'] - w / h) < 0.01 for pg, (w, h) in zip(r['pages'], SAMPLE)), r)


@case
def images(p):
    # W1's order box: d moves the chosen name down (b.png first).
    snaps, info, _, _ = p.run('images', ['a.png', 'b.png'], 'menu', -1, '80x25', ['a.png', 'b.png'], 'i SNAP d SNAP RET SNAP RET WAIT:10 SNAP')
    row = lambda s, w: next(i for i, r in enumerate(s['rows']) if w in r)
    check(row(snaps[1], 'a.png') < row(snaps[1], 'b.png') and row(snaps[2], 'b.png') < row(snaps[2], 'a.png'), 'order box')
    check(snaps[3]['text'] == 'b.pdf', snaps[3]['text'])
    r = p.inspect('images', 'b.pdf')
    # b.png 40 x 80 (no resolution: MuPDF takes 96 dpi), a.png 64 x 48 at 150 dpi.
    check(r['count'] == 2 and [(round(x['w'], 1), round(x['h'], 1)) for x in r['pages']] == [(30.0, 60.0), (30.7, 23.0)], r)


@case
def merge(p):
    snaps, info, _, _ = p.run('merge', ['sample.pdf', 'pages10.pdf'], 'merge', -1, '85x25', ['sample.pdf', 'pages10.pdf'],
                              'SNAP d RET SNAP RET WAIT:10 SNAP')
    screen_ok(snaps, 'merge', 85)
    check('結合する PDF と順番' in text(snaps[1]), 'the order box')
    check(snaps[2]['text'] == 'pages10_merged.pdf', snaps[2]['text'])
    r = p.inspect('merge', 'pages10_merged.pdf')
    check(widths(r) == [w for w, _ in PAGES10] + [w for w, _ in SAMPLE], widths(r))
    check(any('13 ページ' in v for _, v in snaps[3]['info']), snaps[3]['info'])
    # With a password among them: asked for that one.
    snaps, info, _, _ = p.run('merge_locked', ['sample.pdf', 'password.pdf'], 'merge', -1, '80x25', ['sample.pdf', 'password.pdf'],
                              'RET SNAP "secret" RET RET WAIT:10 SNAP')
    check('password.pdf のパスワード' in text(snaps[1]), 'password asked in a merge')
    r = p.inspect('merge_locked', 'sample_merged.pdf')
    check(r['count'] == 4 and r['encrypted'] == 0, r)
    snaps, info, _, _ = p.run('merge_cancel', ['sample.pdf', 'pages10.pdf'], 'merge', -1, '80x25', ['sample.pdf', 'pages10.pdf'], 'ESC SNAP')
    check(snaps[1]['depth'] == 1, 'order box given up')


@case
def repair(p):
    check(p.inspect('..', 'fixtures/badxref.pdf')['repaired'] == 1, 'the fixture needs repairing')
    snaps, info, _, _ = p.run('repair', ['badxref.pdf'], 'menu', -1, '80x25', ['badxref.pdf'], 'f RET RET WAIT:5 SNAP')
    r = p.inspect('repair', 'badxref_repaired.pdf')
    check(r['open'] == 'ok' and r['repaired'] == 0 and widths(r) == [595, 842, 300], (r, snaps[1]['label']))
    # Cut at two thirds (no xref, no catalog): MuPDF cannot make a document of it.
    snaps, info, _, _ = p.run('repair_cut', ['truncated.pdf'], 'menu', -1, '80x25', ['truncated.pdf'], 'f SNAP')
    check('PDF を開けません: truncated.pdf' in snaps[1]['label'] and snaps[1]['depth'] == 1, snaps[1]['label'])
    snaps, info, _, _ = p.run('broken', ['broken.pdf'], 'menu', -1, '80x25', ['broken.pdf'], 'f SNAP')
    check('PDF を開けません: broken.pdf' in snaps[1]['label'] and snaps[1]['depth'] == 1, snaps[1]['label'])


@case
def heavy(p):
    # 1 GB of RAM: 100 pages (each its own image) merged with themselves and
    # compressed, without stopping; Esc in the middle leaves nothing.
    p.run('heavy', ['big100.pdf'], 'menu', -1, '80x25', ['big100.pdf'], 'ESC')
    p.sh('cd %s/check/heavy && cp big100.pdf big100b.pdf' % p.base)
    snaps, info, _, _ = rerun(p, 'heavy', 'merge', ['big100.pdf', 'big100b.pdf'], 'RET RET WAIT:0.3 SNAP WAIT:300 SNAP')
    check(snaps[1]['depth'] == 2 and '結合中' in text(snaps[1]) and '%' in text(snaps[1]), ('progress shown', text(snaps[1])[-800:]))
    r = p.inspect('heavy', 'big100_merged.pdf')
    check(r['count'] == 200, r['count'])
    print('merge 200 pages: %s s, VmHWM %s kB' % (info['TIME'], info['VMHWM']))
    snaps, info, _, _ = rerun(p, 'heavy', 'menu', ['big100_merged.pdf'], 'c RET n RET WAIT:600 SNAP')
    f = p.files('heavy')
    check(f['big100_merged_compressed.pdf']['size'] < f['big100_merged.pdf']['size'], (f['big100_merged.pdf']['size'], f['big100_merged_compressed.pdf']['size']))
    print('compress 200 pages: %s s, VmHWM %s kB, %d -> %d bytes' % (info['TIME'], info['VMHWM'], f['big100_merged.pdf']['size'], f['big100_merged_compressed.pdf']['size']))
    snaps, info, _, _ = rerun(p, 'heavy', 'merge', ['big100.pdf', 'big100b.pdf'], 'RET RET y WAIT:0.5 ESC SNAP WAIT:0 SNAP')
    check('操作を中止しました' in text(snaps[1]) and snaps[1]['depth'] == 1, text(snaps[1])[-300:])
    check(p.files('heavy')['big100_merged.pdf']['size'] == f['big100_merged.pdf']['size'], 'Esc: the old file kept')


@case
def screen(p):
    """The real fdtp (the test build in bin/, never ~/.fdtp) on the Pomera's
    screen through FDTP_TEST_KEYS, at 85 x 25 (its own size) and 80 x 25:
    the menu, the password box, the split's questions and its result. The
    snapshots come back as PNG and .cells at 200 KB/s."""
    keys = ('WAIT:1.5 DOWN M-p WAIT:0.5 SNAP p WAIT:0.3 SNAP "abc" WAIT:0.3 SNAP ESC WAIT:0.3 '
            'M-p WAIT:0.3 s WAIT:0.5 SNAP r WAIT:0.3 SNAP "1,2-" RET WAIT:4 SNAP ESC WAIT:0.5 SNAP q WAIT:0.3 y')
    for size, cols in (('85x25', 85), ('80x25', 80)):
        d = '%s/check/screen%s' % (p.base, size)
        p.sh('rm -rf {d} && mkdir -p {d}/files {d}/snap && cp {b}/fixtures/sample.pdf {d}/files/ && cd {d}/files && '
                   'until {lock} env FDTP_LANGUAGE=ja FDTP_TEST_KEYS={k} FDTP_SNAPSHOT_PREFIX={d}/snap/s HOME={d} LANG=ja_JP.UTF-8 timeout 90 {b}/bin/fdtp {opt} . ; do test $? = 75 || break; done; '
                   'cd {d}/snap && for f in *.ppm; do python3 -I {b}/fixtures/ppm2png.py $f && rm $f; done'.format(
                       d=os.path.expanduser(d), b=os.path.expanduser(p.base), k=shquote(' '.join(keys.split())), lock=LOCK, opt='--size 80x25' if size == '80x25' else ''), check=False)
        local = Path(os.path.expanduser(d)) / 'snap'
        shots = sorted(local.glob('s-*.cells'))
        check(len(shots) == 7, (size, 'snapshots', len(shots)))
        snaps = [json.loads(f.read_text()) for f in shots]
        rows = lambda s: [''.join(chr(c[0]) if c[0] else ' ' for c in r if c[3] != 2) for r in s['rows']]
        t = ['\n'.join(rows(s)) for s in snaps]
        check(all(len(s['rows'][0]) == cols for s in snaps), (size, 'columns'))
        check('PDF のメニュー' in t[0] and '対象 sample.pdf' in t[0] and '画像→PDF' in t[0], (size, 'menu'))
        check('付けるパスワード' in t[1] and '***' in t[2] and 'abc' not in t[2], (size, 'password box'))
        check('分割のしかた' in t[3] and '分けるページの範囲' in t[4], (size, 'split questions'))
        check('分割しました' in t[5] and 'sample_2-3.pdf' in t[5], (size, 'result'))
        check('sample_1.pdf' in t[6] and 'sample_2' in t[6], (size, 'the filer read again'))   # the filer cuts long names
        for i, s in enumerate(snaps):
            for col, row, w, h, rule, kind in s['boxes'][1:]:
                check(col >= 0 and col + w + 1 <= cols and row >= 1 and row + h <= 23, (size, i, 'box', col, row, w, h))
        print('screen %s: %d snapshots' % (size, len(shots)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--deploy', action='store_true'); ap.add_argument('--fdtp', action='store_true')
    ap.add_argument('--only', nargs='+'); ap.add_argument('--fixtures'); ap.add_argument('--list')
    ap.add_argument('--local', action='store_true', help='(on the Pomera) run the cases here')
    args = ap.parse_args()
    if args.fixtures: make_fixtures(args.fixtures); return
    if args.list: remote_list(args.list); return
    if args.local:
        p = Pomera()
        def ppm_of(case, n):
            data = Path(os.path.expanduser('%s/check/%s-%03d.ppm' % (p.base, case, n))).read_bytes()
            magic, wh, maxv, pixels = data.split(b'\n', 3); w, h = map(int, wh.split())
            return lambda x, y: tuple(pixels[(y * w + x) * 3:(y * w + x) * 3 + 3])
        p.ppm = ppm_of
        for name in args.only or CASES:
            CASES[name](p)
            print('%s: ok (%d checks so far)' % (name, checks), flush=True)
        left = p.hidden()
        check(not left, ('hidden temporaries left', left))
        print('PDF menu checks passed: %d checks' % checks)
        return
    # The run from the Mac sends fdtp/ and port/app/platform/ to the build
    # host: only in the private tree (not in the public copy's layout).
    if not (REPO / 'port' / 'app' / 'platform').is_dir():
        print('%s: skipped: not the private layout (fdtp/ beside port/app/platform/), which the build host run needs'
              % Path(__file__).name)
        return
    if args.deploy or args.fdtp: deploy(args.fdtp)
    send_scripts()
    _, pomera = hosts()
    # One ssh for everything on the Pomera: the libraries from the installed
    # fdtp's ~/.fdtp/lib (only read), the fixtures once, then the cases.
    remote = ('cd ~/{d} && rm -f fdtp pdftools-harness && mkdir -p lib check && for l in {libs}; do cmp -s ~/.fdtp/lib/$l lib/$l || cp ~/.fdtp/lib/$l lib/$l; done && '
              '(test -f fixtures/badxref.pdf || (cd fixtures && until {lock} python3 -I check_pdftools.py --fixtures .; do test $? = 75 || exit 1; echo "(lock busy)"; done)) && '
              'python3 -I fixtures/check_pdftools.py --local {only}').format(
        d=POMERA_DIR, libs=' '.join(RUNTIME_LIBS), lock=LOCK, only=('--only ' + ' '.join(args.only)) if args.only else '')
    r = subprocess.run(['ssh', '-o', 'ConnectTimeout=20', '-o', 'ServerAliveInterval=10', '-o', 'ServerAliveCountMax=2', pomera, remote])
    if not args.only or 'screen' in args.only:
        out = ROOT / 'build/pdftools-check'; out.mkdir(parents=True, exist_ok=True)
        subprocess.run(['rsync', '-rt', '--bwlimit=200', '--include=*/', '--include=*.png', '--include=*.cells', '--exclude=*',
                        '%s:%s/check/' % (pomera, POMERA_DIR), str(out) + '/'])
    sys.exit(r.returncode)


if __name__ == '__main__':
    main()
