#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Stage 3 (PDF viewer) acceptance test, run from the Mac.

    check_pdf.py [--deploy] [--build-dir D] [--pomera-dir D] [--measure PDF...]

--deploy  sends the working tree to the build host's ~/work/fdtp-pdf (or
          --build-dir), cross builds the harness (tests/pdf_harness.c +
          src/pdfview.c with libmupdf) and puts it in the Pomera's
          ~/work/fdtp-pdf/bin (or --pomera-dir); the libraries the Pomera
          lacks are copied there from its own ~/.fdtp/lib, never sent.
          --build-only builds, --deploy-only builds and sends.
To the Pomera everything goes in one rsync at 200 KB/s, the tests run in
one ssh under flock -o -w 60 /tmp/pomera-heavy.lock, and the results come
back in one rsync (its Wi-Fi stopped under fast transfers).
The ssh names are FDTP_BUILD_HOST (an amd64 Debian 13 with the armhf cross
compiler and tools/make_sysroot.sh's sysroot; default build-host) and
FDTP_POMERA_HOST (default pomera), from the environment or tests/dev_env.py's
file.
Then the test PDFs (tools/make_test_pdf.py) and tests/pdf_*.keys are run on
the Pomera under flock, the snapshots fetched to build/pdf-check/out and
their pixels checked. Nothing is written to /tmp except the lock.
"""
import argparse
import importlib.util
import inspect
import io
import re
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
spec = importlib.util.spec_from_file_location('make_test_pdf', ROOT / 'tools/make_test_pdf.py')
fixtures = importlib.util.module_from_spec(spec); spec.loader.exec_module(fixtures)
sys.path.insert(0, str(Path(__file__).resolve().parent))
import dev_env  # noqa: E402
BUILD_HOST, POMERA = dev_env.get('FDTP_BUILD_HOST', 'build-host'), dev_env.get('FDTP_POMERA_HOST', 'pomera')
# --build-dir / --pomera-dir (under the hosts' homes) let several workers
# test at once (docs/FDTP_WORK_36-41.md: W4 builds in ~/work/fdtp-src-w4).
BUILD_DIR, POMERA_DIR = 'work/fdtp-pdf', 'work/fdtp-pdf'
# libjpeg/glib/graphite2/pcre2 copied from the Pomera (STAGE3_NOTES.md),
# for resolving libmupdf's dependencies at link time only.
LINKLIB = '$HOME/work/fdtp-pdf/linklib'
RUNTIME_LIBS = ['libmupdf.so.25.1', 'libjbig2dec.so.0', 'libgumbo.so.3', 'libmujs.so.3']
LOCK = 'flock -o -w 60 /tmp/pomera-heavy.lock'
NAVY, YELLOW = (0, 0, 85), (255, 255, 0)


# The Pomera's SDIO Wi-Fi and SD stopped twice under fast transfers and quick
# ssh connections in a row (dma-pl330 "Runtime PM usage count underflow",
# 2026-10-23): everything for it goes in one rsync at 200 KB/s, the tests
# run in one ssh, the results come back in one rsync. A connection silent
# for 20 s ends the run (do not retry).
SSH = 'ssh -o ConnectTimeout=20 -o ServerAliveInterval=10 -o ServerAliveCountMax=2'
RATE = '--bwlimit=200'
STAGE = ROOT / 'build/pdf-check/stage'


def sh(host, command, data=None):
    r = subprocess.run(SSH.split() + [host, command], input=data, capture_output=True)
    if r.returncode:
        sys.exit('%s: %s\n%s%s' % (host, command, r.stdout.decode(errors='replace')[-3000:], r.stderr.decode(errors='replace')[-3000:]))
    return r.stdout


def rsync(*args):
    r = subprocess.run(['rsync', '-rlt', RATE, '-e', SSH] + list(args), capture_output=True)
    if r.returncode: sys.exit('rsync %s\n%s' % (' '.join(args), r.stderr.decode(errors='replace')[-3000:]))


def on_pomera(script):
    """STAGE (at 200 KB/s) to ~/POMERA_DIR, then script there in one ssh
    (under the heavy lock); its output."""
    (STAGE / 'run.sh').write_text(script)
    rsync(str(STAGE) + '/', '%s:%s/' % (POMERA, POMERA_DIR))
    return sh(POMERA, 'cd ~/%s && %s sh run.sh 2>&1; echo "=== EXIT $?"' % (POMERA_DIR, LOCK))


def split_sections(output):
    """on_pomera()'s output by its "=== NAME" lines: {NAME: bytes}."""
    sections, name = {}, 'HEAD'
    for line in output.splitlines(keepends=True):
        if line.startswith(b'=== '):
            name = line[4:].strip().decode()
            if name.startswith('EXIT'): sections['EXIT'] = line; name = 'TAIL'
            continue
        sections[name] = sections.get(name, b'') + line
    return sections


def libraries():
    """Shell lines that put the libraries in lib/ from the Pomera's own copies (never sent)."""
    return 'mkdir -p lib\n' + ''.join('test -f lib/{n} || cp -pL ~/.fdtp/lib/{n} lib/ || cp -pL ~/work/fdtp-pdf/lib/{n} lib/\n'.format(n=n) for n in RUNTIME_LIBS)


def deploy(send=True):
    files = subprocess.run(['git', '-C', str(REPO), 'ls-files', '-z', '--cached', '--others', '--exclude-standard', 'fdtp', 'port/app/platform'],
                           capture_output=True, check=True).stdout.split(b'\0')
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w:gz') as tar:
        for f in files:
            p = REPO / f.decode()
            if f and p.is_file(): tar.add(p, arcname=f.decode())
    sh(BUILD_HOST, 'mkdir -p ~/%s && tar -C ~/%s -m -xzf -' % (BUILD_DIR, BUILD_DIR), buf.getvalue())
    sysroot = '$HOME/work/sysroot-armhf'; lib = sysroot + '/usr/lib/arm-linux-gnueabihf'
    out = '$HOME/%s/fdtp/build-arm-linux-gnueabihf' % BUILD_DIR
    # The note's input line (doc_input) with the test input method of
    # tests/ime_fake.c (romaji, "nihon" -> 日本), as check_doc.py does.
    support = ' '.join('src/%s.c' % m for m in 'pdfview app widgets keys commands canvas cells raster font24 doc_input'.split()) + ' tests/ime_fake.c'
    sh(BUILD_HOST, 'cd ~/{d}/fdtp && mkdir -p {o}/tmp && export TMPDIR={o}/tmp && (test -f {o}/font24.inc || python3 tools/gen_font.py --output {o}/font24.inc) && '
             'arm-linux-gnueabihf-gcc -O2 -std=c11 -Wall -Wextra -Werror -DFDTP_WITH_MUPDF -Isrc -I{o} -I{s}/usr/include -I{s}/usr/include/arm-linux-gnueabihf '
             '{src} tests/pdf_harness.c -o {o}/pdf-harness -L{l} -Wl,-rpath-link,{l} -Wl,-rpath-link,{k} '
             "-Wl,--disable-new-dtags -Wl,-rpath,'$ORIGIN/../lib' -lmupdf -lm 2>&1".format(d=BUILD_DIR, o=out, s=sysroot, l=lib, k=LINKLIB, src=support))
    if not send: return
    # Into STAGE/bin: the next on_pomera() sends it (rsync, 200 KB/s).
    (STAGE / 'bin').mkdir(parents=True, exist_ok=True)
    (STAGE / 'bin/pdf-harness').write_bytes(sh(BUILD_HOST, 'cat %s/pdf-harness' % out))
    (STAGE / 'bin/pdf-harness').chmod(0o755)


# Item 40: the large file, made on the Pomera (its source is sent there).
BIG_VERSION = 'v1'


def big_pdf(path, pages, image):
    """1 page with an image x image RGB gradient, 1 page of 1500 x 1500 noise
    (6.75 MB that does not compress), pages of 60 dense lines of Helvetica,
    and a page of two vertical Japanese lines (UniJIS-UCS2-V)."""
    import os
    import zlib
    words = 'alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma tau upsilon phi chi psi omega'.split()
    objs = []

    def add(body):
        objs.append(body); return len(objs)

    def stream(data, extra=b''):
        z = zlib.compress(data, 6)
        return b'<< /Length %d /Filter /FlateDecode %s>>\nstream\n' % (len(z), extra) + z + b'\nendstream'
    f1 = add(b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>')
    desc = add(b'<< /Type /FontDescriptor /FontName /MS-Mincho /Flags 6 /FontBBox [0 -141 1000 859] /ItalicAngle 0 '
               b'/Ascent 859 /Descent -141 /CapHeight 769 /StemV 78 >>')
    cid = add(b'<< /Type /Font /Subtype /CIDFontType0 /BaseFont /MS-Mincho /CIDSystemInfo << /Registry (Adobe) /Ordering (Japan1) '
              b'/Supplement 2 >> /FontDescriptor %d 0 R /DW 1000 >>' % desc)
    f2 = add(b'<< /Type /Font /Subtype /Type0 /BaseFont /MS-Mincho /Encoding /UniJIS-UCS2-V /DescendantFonts [%d 0 R] >>' % cid)
    pages_num = add(None); kids = []
    page = lambda c, res: kids.append(add(b'<< /Type /Page /Parent %d 0 R /MediaBox [0 0 595 842] /Contents %d 0 R '
                                          b'/Resources << %s >> >>' % (pages_num, c, res)))
    base = bytearray()
    for x in range(image):
        g = 0 if x % 200 < 4 else 1
        base += bytes(((x * 255 // image) * g, 0, 160 * g))
    raw = bytearray(); black = bytes(image * 3)
    for y in range(image):
        if y % 200 < 4: raw += black; continue
        row = bytearray(base); row[1::3] = bytes([y * 255 // image]) * image; raw += row
    img = add(stream(bytes(raw), b'/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB /BitsPerComponent 8 ' % (image, image)))
    del raw
    page(add(stream(b'q 515 0 0 515 40 160 cm /Im1 Do Q BT /F1 24 Tf 40 100 Td (Big image page) Tj ET')),
         b'/Font << /F1 %d 0 R >> /XObject << /Im1 %d 0 R >>' % (f1, img))
    noise = os.urandom(1500 * 1500 * 3)
    img2 = add(b'<< /Length %d /Type /XObject /Subtype /Image /Width 1500 /Height 1500 /ColorSpace /DeviceRGB '
               b'/BitsPerComponent 8 >>\nstream\n' % len(noise) + noise + b'\nendstream')
    del noise
    page(add(stream(b'q 515 0 0 515 40 160 cm /Im2 Do Q BT /F1 24 Tf 40 100 Td (Noise page) Tj ET')),
         b'/Font << /F1 %d 0 R >> /XObject << /Im2 %d 0 R >>' % (f1, img2))
    seed = 12345
    for p in range(pages):
        ops = [b'BT /F1 8 Tf 10 TL 30 812 Td']
        for i in range(60):
            line = []
            for _ in range(14):
                seed = (seed * 1103515245 + 12345) & 0x7fffffff; line.append(words[seed % len(words)])
            ops.append(('(%d.%d %s) Tj T*' % (p + 3, i + 1, ' '.join(line))).encode())
        ops.append(b'ET')
        page(add(stream(b'\n'.join(ops))), b'/Font << /F1 %d 0 R >>' % f1)
    vertical = ['縦書きの文章です', '二行目の縦書き']
    ops = [b'BT /F2 36 Tf %d 760 Td <%s> Tj ET' % (480 - 60 * i, t.encode('utf-16-be').hex().upper().encode()) for i, t in enumerate(vertical)]
    page(add(stream(b'\n'.join(ops))), b'/Font << /F2 %d 0 R >>' % f2)
    objs[pages_num - 1] = b'<< /Type /Pages /Kids [%s] /Count %d >>' % (b' '.join(b'%d 0 R' % k for k in kids), len(kids))
    catalog = add(b'<< /Type /Catalog /Pages %d 0 R >>' % pages_num)
    with open(path, 'wb') as out:
        pos = out.write(b'%PDF-1.4\n%\xe2\xe3\xcf\xd3\n'); offsets = []
        for n, body in enumerate(objs, 1):
            offsets.append(pos); pos += out.write(b'%d 0 obj\n' % n + body + b'\nendobj\n')
        out.write(b'xref\n0 %d\n0000000000 65535 f \n' % (len(objs) + 1))
        for o in offsets: out.write(b'%010d 00000 n \n' % o)
        out.write(b'trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (len(objs) + 1, catalog, pos))


def parse_annots(stdout):
    """pdf-harness --annots: {'pages': n, 'annotate': 0|1, 'list': [(page, subtype, rect, quads, contents)], 'rc': ...}."""
    out = {'list': []}
    for line in stdout.decode().splitlines():
        if line.startswith('PAGES '): out['pages'] = int(line[6:])
        elif line.startswith('PERMISSION annotate '): out['annotate'] = int(line[20:])
        elif line.startswith('RC '): out['rc'] = line[3:]
        elif line.startswith('ERROR '): out['error'] = line[6:]
        elif line.startswith('ANNOT '):
            head, _, contents = line[6:].partition('|'); f = head.split()
            n = int(f[6]); nums = [float(x) for x in f[7:7 + 4 * n]]
            out['list'].append((int(f[0]), f[1], tuple(float(x) for x in f[2:6]), [tuple(nums[4 * k:4 * k + 4]) for k in range(n)], contents))
    return out


def pdf_objects(data):
    """The latest body of each object of a PDF written without object streams."""
    objs = {}
    for m in re.finditer(rb'(?s)(\d+) 0 obj\s*(.*?)\s*endobj', data): objs[int(m.group(1))] = m.group(2)
    return objs


def pdf_string(token):
    """A PDF string token, hex <...> or literal (...), as text (UTF-16BE with its BOM, else Latin-1)."""
    if token.startswith(b'<'): b = bytes.fromhex(token[1:-1].decode())
    else:
        b, i, s = bytearray(), 1, token
        while i < len(s) - 1:
            c = s[i]
            if c == 0x5c:
                i += 1; e = s[i]
                if 0x30 <= e <= 0x37:
                    j = i
                    while j < i + 3 and 0x30 <= s[j] <= 0x37: j += 1
                    b.append(int(s[i:j], 8)); i = j; continue
                b += {ord('n'): b'\n', ord('r'): b'\r', ord('t'): b'\t', ord('b'): b'\b', ord('f'): b'\f'}.get(e, bytes([e]))
            else: b.append(c)
            i += 1
        b = bytes(b)
    return b[2:].decode('utf-16-be') if b.startswith(b'\xfe\xff') else b.decode('latin-1')


def ppm(path):
    data = path.read_bytes(); magic, wh, maxv, pixels = data.split(b'\n', 3)
    w, h = map(int, wh.split()); assert magic == b'P6' and maxv == b'255' and len(pixels) == w * h * 3
    assert (w, h) == (1024, 600), path
    return pixels


def px(im, x, y): return tuple(im[(y * 1024 + x) * 3:(y * 1024 + x) * 3 + 3])


# Revision 1 item 29: the page in the body of the screen's frame, canvas
# x 44..980, y 24..528 at 80 x 25 (raster.h VIEW_LEFT, VIEW_W, VIEW_H).
VIEW_LEFT, VIEW_TOP, VIEW_W, VIEW_H = 44, 24, 936, 504


def layout(size, scale_mode):
    """The viewer's fit placement: (scale, left, top) of the page on the canvas."""
    w, h = size
    s = min(VIEW_W / w, VIEW_H / h) if scale_mode == 'fit' else VIEW_W / w
    dw, dh = w * s, h * s
    left = VIEW_LEFT + ((VIEW_W - dw) / 2 if dw <= VIEW_W else 0)
    top = VIEW_TOP + ((VIEW_H - dh) / 2 if dh <= VIEW_H else 0)
    return s, left, top


def to_canvas(page, mode, x, y):
    """PDF point (y up) to canvas pixel, for a page placed at its top."""
    s, left, top = layout(fixtures.PAGES[page]['size'], mode)
    return int(left + x * s), int(top + (fixtures.PAGES[page]['size'][1] - y) * s)


def rect_centre(page, mode):
    x, y, w, h = fixtures.PAGES[page]['rect']
    return to_canvas(page, mode, x + w / 2, y + h / 2)


def colour_ok(got, rgb):
    want = tuple(round(c * 255) for c in rgb)
    return all(abs(a - b) <= 8 for a, b in zip(got, want))


def dark_ratio(im, box):
    x0, y0, x1, y1 = box; dark = total = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            total += 1; dark += sum(px(im, x, y)) < 200
    return dark / total


def yellow_box(im):
    pts = [(x, y) for y in range(VIEW_TOP, VIEW_TOP + VIEW_H) for x in range(VIEW_LEFT, VIEW_LEFT + VIEW_W) if px(im, x, y) == YELLOW]
    if not pts: return None
    return len(pts), min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts)


def text_box(t, mode):
    """Rough canvas box of a text line: baseline - size .. baseline + size/4."""
    x, y = t['pos']; size = t['size']
    x0, y0 = to_canvas(t['page'], mode, x, y + size)
    x1, y1 = to_canvas(t['page'], mode, x + size * len(t['text']) * (1 if ord(t['text'][0]) > 127 else 0.5), y - size / 4)
    return x0, y0, x1, y1


def parse(stdout):
    snaps, ticks, info = [], [], {}
    for line in stdout.decode().splitlines():
        if line.startswith('SNAP '):
            # row0: the frame's top line (the name) and bottom line (row 22:
            # the mode, the zoom, ページ N/M), as one.
            head, row0, row22, row24, row23 = line[5:].split('|', 4)
            n, depth = map(int, head.split()); snaps.append({'n': n, 'depth': depth, 'row0': row0 + ' ' + row22, 'row24': row24, 'row23': row23})
        elif line.startswith('TICK '): ticks.append(float(line[5:]))
        else:
            k, _, v = line.partition(' '); info[k] = v
    return snaps, ticks, info


# ---- Revision 1 items 39 (Alt+p) and 40 (the writing mode) ----

# Helvetica's widths (its AFM) for the characters of LATIN, to find the
# words of page 1 in page points.
HELV = dict(zip('Hello FDTPsarchtg', [722, 556, 222, 222, 556, 278, 611, 722, 611, 667, 500, 556, 333, 500, 556, 278, 556]))


def word_box(word):
    """fz box (x0, x1) of a word of LATIN on page 1."""
    text = fixtures.LATIN['text']; at = text.index(word)
    x = fixtures.LATIN['pos'][0] + sum(HELV[c] for c in text[:at]) * fixtures.LATIN['size'] / 1000
    return x, x + sum(HELV[c] for c in word) * fixtures.LATIN['size'] / 1000


def canvas_box(cols, page, x0, y0, x1, y1):
    """A fz box of page (y down from the top) on the canvas, page fitted, at cols x 25."""
    grid_x = (1024 - cols * 12) // 2; left0, vw, vh = grid_x + 12, (cols - 2) * 12, 21 * 24
    w, h = fixtures.PAGES[page]['size']; s = min(vw / w, vh / h)
    left, top = left0 + (vw - w * s) / 2, 24 + (vh - h * s) / 2
    return int(left + x0 * s), int(top + y0 * s), int(left + x1 * s) + 1, int(top + y1 * s) + 1


def count(im, box, pred):
    x0, y0, x1, y1 = box
    return sum(1 for y in range(y0, y1) for x in range(x0, x1) if pred(px(im, x, y)))


def near(rgb, d=12): return lambda p: all(abs(a - b) <= d for a, b in zip(p, rgb))


YELLOWISH, REDDISH = near((255, 255, 0), 30), lambda p: p[0] > 170 and p[1] < 70 and p[2] < 70
BLUISH = lambda p: p[2] > 200 and p[0] < 60 and p[1] < 140


def check_writing(results, raw, annots, unchanged, saved, src, shot, check):
    line_y = 842 - fixtures.LATIN['pos'][1]          # LATIN's baseline, fz space
    text_y = (line_y - 28, line_y + 10)
    fdtp, search, target = word_box('FDTP'), word_box('search'), word_box('target')

    def annot_case(case, cols):
        snaps, ticks, info = results[case]
        check(info.get('RC') == '0' and len(snaps) == 23 and info.get('HISTORY') == '0', (case, info, len(snaps)))
        s = {x['n']: x for x in snaps}
        rows = {n: s[n]['row23'].strip() for n in s}
        want = {2: '書き込み: 字 1/39', 3: '書き込み: 字 7/39', 4: '書き込み: 4 字を選んでいます', 5: 'ハイライト', 6: '保存しました',
                7: '取り消し線', 8: '書き込み: 12 字を選んでいます', 9: '下線', 11: '付箋: 日本memo', 15: '書き込み: 字 17/39',
                16: 'ハイライト', 17: '下線', 18: '付箋: 日本memo', 19: '付箋: 日本めも', 21: '保存しました'}
        for n, text in want.items(): check(rows[n] == text, (case, n, rows[n], text))
        check('e書込' in s[1]['row24'] and 'e書込' in s[22]['row24'] and 'hハイライト' in s[2]['row24'] and 'C-s保存' in s[2]['row24'],
              (case, 'bands', s[1]['row24'], s[2]['row24']))
        # The bands end before 描画中 (the last 7 columns) at this width.
        for n in (1, 2, 10): check(not s[n]['row24'][cols - 8:].strip(), (case, 'band width', n, s[n]['row24']))
        check('変更あり' in s[5]['row0'] and '変更あり' not in s[6]['row0'] and '変更あり' in s[7]['row0'] and '変更あり' not in s[21]['row0'],
              (case, 'dirty', s[5]['row0'], s[6]['row0']))
        check(rows[10].startswith('付箋: 日本') and 'RET貼る' in s[10]['row24'] and '[あ]' in s[10]['row24'], (case, 10, rows[10], s[10]['row24']))
        check(rows[12].endswith('INFO 付箋|日本memo'), (case, 12, rows[12]))
        check(rows[13].endswith('ASK この取り消し線を消しますか？') and rows[14] == '取り消し線', (case, 13, rows[13], rows[14]))
        check(rows[20].endswith('HELP PDF のキー') and s[23]['depth'] == 0, (case, 20, rows[20]))
        box = lambda w: canvas_box(cols, 0, w[0], text_y[0], w[1], text_y[1])
        im = shot(case, 3); x0, y0, x1, y1 = box(fdtp)
        check(count(im, (x0 - 6, y0 - 6, x1, y1 + 6), near((0xe6, 0, 0x78), 2)) > 20, (case, 'the cursor frame'))
        check(count(shot(case, 4), box(fdtp), near((175, 212, 255), 6)) > 100, (case, 'the selection tint'))
        check(count(shot(case, 2), box(fdtp), YELLOWISH) == 0, (case, 'no highlight before h'))
        hl = count(shot(case, 5), box(fdtp), YELLOWISH); area = (box(fdtp)[2] - box(fdtp)[0]) * (box(fdtp)[3] - box(fdtp)[1])
        check(hl > area * 0.3, (case, 'highlight drawn', hl, area))
        check(count(shot(case, 7), box(search), REDDISH) > 15 and count(shot(case, 15), box(search), REDDISH) == 0, (case, 'strike-out drawn, then deleted'))
        check(count(shot(case, 9), box(target), BLUISH) > 15, (case, 'underline drawn'))
        # The candidate list (blue cells) copied over the page above row 23.
        check(count(shot(case, 10), (0, 24 * 18, 1024, 24 * 23), near((0, 0, 255), 0)) > 1000, (case, 'candidates over the page'))
        # The file: one highlight on FDTP, an underline over two lines, the
        # note; the strike-out deleted.
        a = annots[case]['list']
        kinds = sorted(k[1] for k in a)
        check(annots[case].get('rc') == '0' and kinds == ['Highlight', 'Text', 'Underline'], (case, a))
        by = {k[1]: k for k in a}
        q = by['Highlight'][3]
        check(len(q) == 1 and abs(q[0][0] - fdtp[0]) < 0.6 and abs(q[0][2] - fdtp[1]) < 0.6 and text_y[0] < q[0][1] < line_y < q[0][3] < text_y[1],
              (case, 'highlight quad', q, fdtp))
        q = by['Underline'][3]
        check(len(q) == 2 and abs(q[0][0] - target[0]) < 0.6 and abs(q[0][2] - target[1]) < 0.6 and
              abs(q[1][0] - 72) < 0.6 and abs(q[1][2] - (72 + 6 * 36)) < 0.6, (case, 'underline quads', q))
        t = by['Text']
        # The note's icon from the top left of の, the 6th character of ゴシック体の文字.
        check(t[4] == '日本めも' and abs(t[2][0] - (72 + 5 * 36)) < 3, (case, 'note', t))
        return ticks

    ticks = annot_case('annot', 80) + annot_case('annot85', 85)
    check(annots['annot']['list'] == annots['annot85']['list'], 'the same annotations at 85 x 25')

    # The saved file, read here: the original bytes untouched, two appended
    # updates chained by /Prev, standard annotations with appearances.
    data, orig = saved['annot.pdf'], (src / 'sample.pdf').read_bytes()
    check(data[:len(orig)] == orig, 'incremental: the original bytes stay')
    starts = [int(x) for x in re.findall(rb'startxref\s+(\d+)', data)]
    prevs = [int(x) for x in re.findall(rb'/Prev (\d+)', data)]
    check(len(starts) == 3 and prevs == starts[:2] and all(data[p:p + 4] == b'xref' for p in starts), ('xref chain', starts, prevs))
    objs = pdf_objects(data)
    page = next(b for n, b in objs.items() if b.startswith(b'<</Type/Page/') and b'/Annots' in b)
    refs = [int(x) for x in re.findall(rb'(\d+) 0 R', re.search(rb'/Annots\[([^\]]*)\]', page).group(1))]
    subtypes = {r: re.search(rb'/Subtype/(\w+)', objs[r]).group(1).decode() for r in refs}
    check(sorted(subtypes.values()) == ['Highlight', 'Popup', 'Text', 'Underline'], ('/Annots', subtypes))
    for r, kind in subtypes.items():
        body = objs[r]
        if kind == 'Popup':
            parent = int(re.search(rb'/Parent (\d+) 0 R', body).group(1)); check(subtypes.get(parent) == 'Text', ('popup parent', parent))
            continue
        check(b'/AP<</N ' in body, (kind, 'has an appearance stream'))
        # With /Resources (macOS PDFKit draws one without it wrongly).
        n = int(re.search(rb'/AP<</N (\d+) 0 R', body).group(1))
        check(re.match(rb'<<[^>]*/Resources', objs[n]) or b'/Resources' in objs[n].split(b'stream')[0], (kind, 'appearance /Resources', objs[n][:200]))
        if kind in ('Highlight', 'Underline'):
            nums = re.search(rb'/QuadPoints\[([^\]]*)\]', body).group(1).split()
            check(len(nums) == (8 if kind == 'Highlight' else 16), (kind, nums))
        else:
            token = re.search(rb'/Contents(<[0-9A-Fa-f]*>|\((?:\\.|[^\\)])*\))', body).group(1)
            check(token.upper().startswith(b'<FEFF') and pdf_string(token) == '日本めも', ('UTF-16 contents', token))
    check(b'StrikeOut' not in b''.join(objs[r] for r in refs), 'the deleted strike-out is not on the page')

    # Opened again: the annotations come from the file.
    snaps, _, info = results['reopen']; s = {x['n']: x for x in snaps}
    check(info.get('RC') == '0' and [s[n]['row23'].strip() for n in (2, 3, 4)] == ['ハイライト', '下線', '付箋: 日本めも'] and s[5]['depth'] == 0,
          ('reopen', [x['row23'] for x in snaps]))
    im = shot('reopen', 1); box = lambda w: canvas_box(80, 0, w[0], text_y[0], w[1], text_y[1])
    check(count(im, box(fdtp), YELLOWISH) > 100 and count(im, box(target), BLUISH) > 15 and count(im, box(search), REDDISH) == 0, 'reopen pixels')

    # Leaving with changes: (N) keeps the file, Esc stays, (S) saves.
    snaps, _, info = results['quit']; s = {x['n']: x for x in snaps}
    check(info.get('RC') == '0' and s[2]['row23'].endswith('ASK 書き込みを保存しますか？') and 'ASK' not in s[3]['row23'] and
          s[3]['depth'] == 1 and '変更あり' in s[3]['row0'] and 'ASK' in s[4]['row23'] and s[5]['depth'] == 0, ('quit', snaps))
    check('same' in unchanged, ('quit (N) left the file as it was', unchanged))
    snaps, _, info = results['quitsave']
    check(info.get('RC') == '0' and 'ASK' in snaps[0]['row23'] and snaps[1]['depth'] == 0 and
          [k[1] for k in annots['quitsave']['list']] == ['Underline'], ('quit (S)', snaps, annots['quitsave']))

    # Alt+p: pdftools_open(PDFTOOLS_MENU, the page) with the file as the selection.
    events = [l for l in raw['tools'].splitlines() if l.startswith(('SNAP ', 'PDFTOOLS '))]
    tools_at = [i for i, l in enumerate(events) if l.startswith('PDFTOOLS')]
    snap_at = {int(l.split()[1]): i for i, l in enumerate(events) if l.startswith('SNAP ')}
    between = lambda a, b: sum(1 for i in tools_at if snap_at.get(a, -1) < i < snap_at[b])
    first = events[tools_at[0]] if tools_at else ''
    check(len(tools_at) == 3 and between(0, 1) == 1 and between(1, 2) == 0 and between(2, 3) == 0 and between(3, 4) == 1 and between(4, 5) == 1,
          ('PDFTOOLS calls', events))
    check(re.match(r'PDFTOOLS first=0 page=1 dir=/\S+/check current=tools\.pdf count=1 name=tools\.pdf$', first), first)
    snaps, _, info = results['tools']; s = {x['n']: x for x in snaps}
    check('まだ作っていません' in s[1]['row23'] and 'ASK 書き込みを保存してから PDF のメニューを開きますか？' in s[2]['row23'] and
          '変更あり' in s[4]['row0'] and '変更あり' not in s[5]['row0'] and s[6]['depth'] == 0, ('tools', snaps))
    check([(k[0], k[1]) for k in annots['tools']['list']] == [(1, 'Highlight')], ('tools saved', annots['tools']))

    # A user password: asked in a box showing only '*', never remembered.
    snaps, _, info = results['password']; s = {x['n']: x for x in snaps}
    inputs = {n: s[n]['row23'].split(' INPUT ', 1)[1] if ' INPUT ' in s[n]['row23'] else None for n in s}
    check(info.get('RC') == '0' and inputs[1] == 'パスワード|' and inputs[2] == 'パスワード|****' and inputs[3] == 'パスワード|***' and
          inputs[4] == 'パスワードが違います。もう一度|' and inputs[5] is None and 'ページ 1/1' in s[5]['row0'], ('password box', inputs))
    check('この PDF は注釈の書き込みが許されていません' in s[6]['row23'] and s[7]['depth'] == 0 and info.get('HISTORY') == '0', ('password', snaps))
    check('nope' not in raw['password'] and 'secret' not in raw['password'] and 'nop' not in raw['password'], 'the password is never shown')
    snaps, _, info = results['password_esc']
    check(info.get('RC') == '0' and snaps[1]['depth'] == 0, ('Esc at the password', snaps))
    # The owner password allows annotations; the file stays encrypted.
    snaps, _, info = results['owner']
    check(info.get('RC') == '0' and snaps[0]['row23'].strip() == 'ハイライト' and snaps[1]['row23'].strip() == '保存しました', ('owner', snaps))
    # mupdf's permission is the file's /P whatever the password (so pdfview
    # lets the owner write itself): 0 with both passwords.
    check([k[1] for k in annots['owner']['list']] == ['Highlight'] and annots['owner']['annotate'] == 0 and
          annots['owner_user']['list'] == annots['owner']['list'] and annots['owner_user']['annotate'] == 0, ('owner file', annots['owner'], annots['owner_user']))
    data, orig = saved['owner.pdf'], (src / 'password.pdf').read_bytes()
    trailers = re.findall(rb'trailer\s*<<(.*?)>>\s*startxref', data, re.S)
    check(data[:len(orig)] == orig and b'/Subtype/Highlight' in data[len(orig):] and b'/Encrypt' in trailers[-1], 'owner: appended, encrypted')

    # A repaired file is written whole (beside it, then renamed): one xref,
    # the annotation, and no work file left.
    snaps, _, info = results['repaired']; s = {x['n']: x for x in snaps}
    check(info.get('RC') == '0' and s[3]['row23'].strip() == 'ハイライト' and s[4]['row23'].strip() == '保存しました（全体を書き直しました）' and
          s[5]['depth'] == 0, ('repaired', snaps))
    check([k[1] for k in annots['repaired']['list']] == ['Highlight'] and annots['repaired']['pages'] == 3 and
          saved['repaired.pdf'].count(b'startxref') == 1 and b'999999' not in saved['repaired.pdf'], ('repaired file', annots['repaired']))
    check('fdtp-' not in unchanged.split('LEFT')[-1], ('no work file left', unchanged))
    repaired = 'written whole, %d pages' % annots['repaired']['pages']

    # A page without text: the point cursor and a note.
    snaps, _, info = results['notext']; s = {x['n']: x for x in snaps}
    empty = '書き込み: このページに文字はありません（付箋は貼れます）'
    check(s[1]['row23'].strip() == empty and 'ページ 3/3' in s[1]['row0'] and '範囲を選んでください' in s[2]['row23'] and
          s[3]['row23'].strip() == '付箋: abc' and s[4]['row23'].strip() == '付箋: zz' and s[5]['row23'].endswith('ASK この付箋を消しますか？') and
          s[6]['row23'].strip() == empty and s[7]['row23'].strip() == '保存しました' and s[8]['depth'] == 0, ('notext', snaps))
    check([(k[0], k[1], k[4]) for k in annots['notext']['list']] == [(2, 'Text', 'abc')], ('notext file', annots['notext']))
    # The deleted note took its popup with it.
    objs = pdf_objects(saved['notext.pdf'])
    page = next(b for b in objs.values() if b.startswith(b'<</Type/Page/') and b'/Annots' in b)
    refs = [int(x) for x in re.findall(rb'(\d+) 0 R', re.search(rb'/Annots\[([^\]]*)\]', page).group(1))]
    kinds = sorted(re.search(rb'/Subtype/(\w+)', objs[r]).group(1).decode() for r in refs)
    check(kinds == ['Popup', 'Text'], ('notext /Annots', kinds))

    # The large file: no stall, little memory, the vertical page.
    snaps, big_ticks, info = results['big']; s = {x['n']: x for x in snaps}
    rows = {n: s[n]['row23'].strip() for n in s}
    m = re.match(r'書き込み: 字 1/(\d+)$', rows[3])
    check(info.get('RC') == '0' and len(snaps) == 12 and m and int(m.group(1)) > 3000, ('big', rows))
    check([rows[n] for n in (4, 5, 6, 7, 8, 9, 10, 11)] == ['ハイライト', '保存しました', '書き込み: 字 1/15', '書き込み: 字 3/15', '書き込み: 字 11/15',
                                                             '書き込み: 字 3/15', '下線', '保存しました'] and 'ページ 1503/1503' in s[6]['row0'] and s[12]['depth'] == 0, ('big rows', rows))
    check(max(big_ticks) < 5 and int(info['VMHWM']) < 200000, ('big time and memory', big_ticks, info['VMHWM']))
    a = annots['big']['list']
    check(len(a) == 2 and a[0][:2] == (2, 'Highlight') and len(a[0][3]) == 11 and a[1][:2] == (1502, 'Underline') and len(a[1][3]) == 1, ('big file', a))
    qx0, qy0, qx1, qy1 = a[1][3][0]
    check(qy1 - qy0 > 2.5 * (qx1 - qx0), ('one quad down the column', a[1][3]))
    sizes = unchanged.split('prefix')[1].split()[:2] if 'prefix' in unchanged else []
    check(len(sizes) == 2 and 0 < int(sizes[0]) - int(sizes[1]) < 100000, ('big: appended only', unchanged))

    # Every character pdfview.c writes is in the font.
    check('MISSING' not in unchanged and re.search(r'GLYPHS \d+', unchanged), ('glyphs', unchanged))

    return ('writing: annot max tick %.3f s; big file: max tick %.3f s, VmHWM %s kB, %s bytes; repaired: %s' %
            (max(ticks), max(big_ticks), info['VMHWM'], sizes[0] if sizes else '?', repaired))


def main():
    global BUILD_DIR, POMERA_DIR
    ap = argparse.ArgumentParser(); ap.add_argument('--deploy', action='store_true')
    ap.add_argument('--deploy-only', action='store_true', help='build and send the harness, run nothing')
    ap.add_argument('--build-only', action='store_true', help='build the harness on the build host, send nothing to the Pomera')
    ap.add_argument('--build-dir', default=BUILD_DIR, help='under the build host\'s home (default %(default)s)')
    ap.add_argument('--pomera-dir', default=POMERA_DIR, help='under the Pomera\'s home (default %(default)s)')
    ap.add_argument('--recheck', action='store_true', help='check the last run again (build/pdf-check), nothing on the Pomera')
    ap.add_argument('--measure', nargs='+', type=Path, metavar='PDF', help='only time page 1 of these files on the Pomera')
    args = ap.parse_args()
    # The run from the Mac sends fdtp/ and port/app/platform/ to the build
    # host: only in the private tree (not in the public copy's layout).
    if not (REPO / 'port' / 'app' / 'platform').is_dir():
        print('%s: skipped: not the private layout (fdtp/ beside port/app/platform/), which the build host run needs'
              % Path(__file__).name)
        return
    BUILD_DIR, POMERA_DIR = args.build_dir, args.pomera_dir
    if args.build_only:
        deploy(send=False); return
    if STAGE.exists() and not args.recheck: shutil.rmtree(STAGE)
    STAGE.mkdir(parents=True, exist_ok=True)
    if args.deploy or args.deploy_only: deploy()
    if args.deploy_only:
        on_pomera(libraries()); return
    if args.measure:
        (STAGE / 'real').mkdir()
        (STAGE / 'real/measure.keys').write_text('WAIT:0 SNAP')
        script = libraries() + 'export FDTP_LANGUAGE=ja\ncd real\n'
        for i, p in enumerate(args.measure):
            shutil.copy(p, STAGE / 'real' / ('m%d.pdf' % i))
            script += 'echo "=== CASE %d"; ../bin/pdf-harness m%d.pdf measure.keys m%d; echo "RC $?"; rm -f m%d.pdf m%d-*.ppm\n' % (i, i, i, i, i)
        sections = split_sections(on_pomera(script))
        for i, p in enumerate(args.measure):
            _, ticks, info = parse(sections.get('CASE %d' % i, b''))
            print('%s: open %s s, page 1 %s s, VmHWM %s kB %s' % (p.name, info.get('OPEN'), ticks[0] if ticks else '-', info.get('VMHWM'), info.get('NOTICE', '')))
        return
    out = ROOT / 'build/pdf-check'; snapdir = out / 'out'
    if snapdir.exists() and not args.recheck: shutil.rmtree(snapdir)
    snapdir.mkdir(parents=True, exist_ok=True)
    src = fixtures.generate(STAGE / 'check')
    for p in sorted((ROOT / 'tests').glob('pdf_*.keys')): shutil.copy(p, STAGE / 'check' / p.name)
    # Item 40: the strings of pdfview.c, for the font check (--glyphs).
    literals = '\n'.join(re.findall(r'"((?:[^"\\\n]|\\.)*)"', (ROOT / 'src/pdfview.c').read_text(encoding='utf-8')))
    (STAGE / 'check/pdfview-strings.txt').write_text(literals, encoding='utf-8')
    # Item 40: a file mupdf opens only by repairing it (startxref points
    # nowhere), so it cannot be saved incrementally.
    sample = (src / 'sample.pdf').read_bytes()
    (STAGE / 'check/repaired.pdf').write_bytes(re.sub(rb'startxref\s+\d+', b'startxref\n999999', sample))
    # Item 40: the files the writing tests change are copies; the large one is
    # made on the Pomera once (9 MB is not sent).
    big = 'big-%s.pdf' % BIG_VERSION
    (STAGE / 'gen_big.py').write_text(inspect.getsource(big_pdf) + '\nimport sys\nbig_pdf(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))\n',
                                      encoding='utf-8')
    script = libraries() + 'export FDTP_LANGUAGE=ja\n' + ('test -f {b} || {{ python3 gen_big.py {b}.tmp 1500 4000 && mv {b}.tmp {b}; }}\n'
                            'cd check && rm -f *.ppm\n'
                            'for n in annot annot85 quit quitsave tools notext; do cp sample.pdf $n.pdf; done\n'
                            'cp password.pdf owner.pdf && cp ../{b} big.pdf\n').format(b=big)
    cases = [('basic', 'sample.pdf', 'pdf_basic.keys'), ('search', 'sample.pdf', 'pdf_search.keys'),
             ('broken', 'broken.pdf', 'pdf_open.keys'),
             ('truncated', 'truncated.pdf', 'pdf_truncated.keys'), ('updown', 'sample.pdf', 'pdf_updown.keys'),
             ('annot', 'annot.pdf', 'pdf_annot.keys'), ('reopen', 'annot.pdf', 'pdf_reopen.keys'),
             ('annot85', 'annot85.pdf', 'pdf_annot.keys'), ('quit', 'quit.pdf', 'pdf_quit.keys'),
             ('quitsave', 'quitsave.pdf', 'pdf_quit_save.keys'), ('tools', '$PWD/tools.pdf', 'pdf_tools.keys'),
             ('password', 'password.pdf', 'pdf_password.keys'), ('password_esc', 'password.pdf', 'pdf_password_esc.keys'),
             ('owner', 'owner.pdf', 'pdf_owner.keys'), ('repaired', 'repaired.pdf', 'pdf_repaired.keys'),
             ('notext', 'notext.pdf', 'pdf_notext.keys'), ('big', 'big.pdf', 'pdf_big.keys')]
    for name, pdf, keys in cases:
        env = 'FDTP_HARNESS_SIZE=85x25 ' if name.endswith('85') else ''
        script += 'echo "=== CASE {n}"; {e}../bin/pdf-harness {p} {k} {n}; echo "RC $?"\n'.format(e=env, p=pdf, k=keys, n=name)
    annot_files = (('annot', 'annot.pdf', ''), ('annot85', 'annot85.pdf', ''), ('quitsave', 'quitsave.pdf', ''),
                   ('tools', 'tools.pdf', ''), ('owner', 'owner.pdf', 'owner'), ('owner_user', 'owner.pdf', 'secret'),
                   ('repaired', 'repaired.pdf', ''), ('notext', 'notext.pdf', ''), ('big', 'big.pdf', ''))
    for name, pdf, password in annot_files:
        script += 'echo "=== ANNOTS {n}"; ../bin/pdf-harness --annots {p} {w}; echo "RC $?"\n'.format(n=name, p=pdf, w=password)
    script += ('echo "=== MISC"; cmp quit.pdf sample.pdf && echo same; cmp -n $(stat -c %s ../{b}) big.pdf ../{b} && echo prefix\n'
               'stat -c %s big.pdf ../{b}; ../bin/pdf-harness --glyphs pdfview-strings.txt; echo LEFT; ls\n').format(b=big)
    # Only the snapshots the checks look at come back (all of them, 118
    # PPMs = 209 MB, kept the Pomera's Wi-Fi busy for minutes).
    keep = ['basic-%03d.ppm' % n for n in (1, 2, 3, 5, 6, 7, 12)] + ['search-%03d.ppm' % n for n in range(2, 8)] + \
           ['updown-%03d.ppm' % n for n in (5, 6, 7, 8, 10)] + ['reopen-001.ppm'] + \
           ['%s-%03d.ppm' % (c, n) for c in ('annot', 'annot85') for n in (2, 3, 4, 5, 7, 9, 10, 15)]
    (STAGE / 'check/keep.txt').write_text('\n'.join(keep) + '\n')
    script += 'for f in *.ppm; do grep -qx "$f" keep.txt || rm -f "$f"; done\n'
    if not args.recheck:
        (out / 'run.txt').write_bytes(on_pomera(script))
    sections = split_sections((out / 'run.txt').read_bytes())
    if 'MISC' not in sections: sys.exit('the run on the Pomera stopped:\n' + b''.join(sections.values()).decode(errors='replace')[-3000:])
    results = {name: parse(sections['CASE ' + name]) for name, _, _ in cases}
    raw = {name: sections['CASE ' + name].decode() for name, _, _ in cases}
    annots = {name: parse_annots(sections['ANNOTS ' + name]) for name, _, _ in annot_files}
    unchanged = sections['MISC'].decode()
    # The snapshots and the saved files, in one rsync (compressed, 200 KB/s).
    if not args.recheck: rsync('-z', '--include=*.ppm', '--include=annot.pdf', '--include=owner.pdf', '--include=repaired.pdf', '--include=notext.pdf',
          '--exclude=*', '%s:%s/check/' % (POMERA, POMERA_DIR), str(snapdir) + '/')
    saved = {n: (snapdir / n).read_bytes() for n in ('annot.pdf', 'owner.pdf', 'repaired.pdf', 'notext.pdf')}
    shot = lambda case, n: ppm(snapdir / ('%s-%03d.ppm' % (case, n)))
    checks = 0

    def check(cond, what):
        nonlocal checks
        assert cond, what; checks += 1

    # Opening failures: a notice, no screen. (A user password is asked for
    # since revision 1 item 40: the password case is below.)
    for name, notice in (('broken', 'PDF を開けません'),):
        snaps, ticks, info = results[name]
        check(info.get('RC') == '3' and info.get('NOTICE') == notice and not snaps, (name, info))

    # A cut file (the catalog is lost): either repaired pages or the open notice,
    # never a crash.
    snaps, ticks, info = results['truncated']
    check((info.get('RC') == '0' and len(snaps) == 3 and snaps[-1]['depth'] == 0) or
          (info.get('RC') == '3' and info.get('NOTICE') == 'PDF を開けません'), ('truncated', info, snaps))
    print('truncated.pdf:', info.get('NOTICE') or ' / '.join(x['row0'].strip() + ' ' + x['row23'].strip() for x in snaps[:2]))

    snaps, ticks, info = results['basic']
    check(info.get('RC') == '0', info)
    check(len(snaps) == 13, len(snaps))
    s = {x['n']: x for x in snaps}
    check('描画中' in s[1]['row24'] and '描画中' not in s[2]['row24'], 'drawing mark')
    check('sample.pdf' in s[2]['row0'] and 'ページ 1/3' in s[2]['row0'] and '全体' in s[2]['row0'], s[2]['row0'])
    im = shot('basic', 1); check(px(im, 512, 300) == NAVY, 'nothing drawn before the first tick')
    im = shot('basic', 2)
    check(colour_ok(px(im, *rect_centre(0, 'fit')), fixtures.PAGES[0]['rgb']), ('page 1 red', px(im, *rect_centre(0, 'fit'))))
    check(px(im, 10, 300) == NAVY and px(im, 1010, 300) == NAVY, 'navy margins')
    check(px(im, *to_canvas(0, 'fit', 20, 20)) == (255, 255, 255), 'white page')
    for t in (fixtures.LATIN, fixtures.MINCHO, fixtures.GOTHIC):
        ratio = dark_ratio(im, text_box(t, 'fit'))
        check(ratio > 0.05, ('text drawn', t['text'], ratio))
    im = shot('basic', 3)
    check('幅' in s[3]['row0'], s[3]['row0'])
    check(colour_ok(px(im, *rect_centre(0, 'width')), fixtures.PAGES[0]['rgb']), 'fit width red')
    check(px(im, VIEW_LEFT + 2, 300) == (255, 255, 255) and px(im, VIEW_LEFT - 2, 300) == NAVY, 'fit width fills the frame\'s width')
    im = shot('basic', 5)
    check('描画中' in s[5]['row24'] and colour_ok(px(im, *rect_centre(0, 'fit')), fixtures.PAGES[0]['rgb']), 'old page kept while drawing')
    im = shot('basic', 6); check('2/3' in s[6]['row0'], s[6]['row0'])
    check(colour_ok(px(im, *rect_centre(1, 'fit')), fixtures.PAGES[1]['rgb']), ('page 2 blue', px(im, *rect_centre(1, 'fit'))))
    im = shot('basic', 7); check('3/3' in s[7]['row0'], s[7]['row0'])
    check(colour_ok(px(im, *rect_centre(2, 'fit')), fixtures.PAGES[2]['rgb']), ('page 3 green', px(im, *rect_centre(2, 'fit'))))
    check('1/3' in s[8]['row0'], s[8]['row0'])
    check('3/3' in s[9]['row0'], s[9]['row0'])
    check('3/3' in s[10]['row0'] and 'ページ番号が違います' in s[10]['row23'], (s[10]['row0'], s[10]['row23']))
    scale3 = layout(fixtures.PAGES[2]['size'], 'fit')[0]
    check('%.0f%%' % (scale3 * 125) in s[11]['row0'], s[11]['row0'])
    im = shot('basic', 12)
    check(sum(px(im, x, 300) == (255, 255, 255) for x in range(1024)) > 500, 'zoomed page fills the view')
    check(s[13]['depth'] == 0, 'q leaves the viewer')
    # ticks: [0]=first page render; the rest are re-renders.
    first_tick = ticks[0]
    check(len(ticks) == 10, ('ticks', ticks))

    snaps, ticks2, info2 = results['search']
    check(info2.get('RC') == '0', info2)
    s = {x['n']: x for x in snaps}
    check('描画中' in s[1]['row24'], 'search renders deferred')
    for n, page, t in ((2, 0, fixtures.LATIN), (3, 1, fixtures.LATIN2), (4, 0, fixtures.LATIN)):
        im = shot('search', n); yb = yellow_box(im)
        check(yb and yb[0] > 40, ('frame', n, yb))
        bx = text_box(t, 'fit')
        check(bx[0] - 4 <= yb[1] and yb[3] <= bx[2] + 4 and bx[1] - 4 <= yb[2] and yb[4] <= bx[3] + 4, ('frame on the text', n, yb, bx))
        check('%d/3' % (page + 1) in s[n]['row0'], s[n]['row0'])
    check('先頭から検索しました' in s[4]['row23'], s[4]['row23'])
    # At fit width the line is below the first screen: the hit is scrolled to the middle.
    yb = yellow_box(shot('search', 5)); bx = text_box(fixtures.LATIN, 'width')
    middle = VIEW_TOP + VIEW_H // 2   # 276: the middle of the frame's body
    check(yb and bx[0] - 4 <= yb[1] and yb[3] <= bx[2] + 4 and abs((yb[2] + yb[4]) / 2 - middle) < 12, ('FDTP at fit width', yb, bx))
    yb = yellow_box(shot('search', 6))
    check(yb and yb[1] < 512 < yb[3] and yb[2] < middle < yb[4], ('zoomed hit revealed in the middle', yb))
    check('見つかりません' in s[7]['row23'] and yellow_box(shot('search', 7)) is None, s[7]['row23'])

    # Revision 1 §3 14: ↑↓ scroll, and turn pages at the edges.
    snaps, _, info3 = results['updown']
    check(info3.get('RC') == '0' and len(snaps) == 11, ('updown', info3, len(snaps)))
    s = {x['n']: x for x in snaps}
    for n, page in ((1, 1), (2, 2), (3, 1), (4, 1), (5, 1), (6, 1), (7, 1), (8, 1), (9, 2), (10, 1)):
        check('%d/3' % page in s[n]['row0'], ('updown page', n, s[n]['row0']))
    view = lambda n: shot('updown', n)[VIEW_TOP * 1024 * 3:(VIEW_TOP + VIEW_H) * 1024 * 3]
    check(view(6) != view(5) and view(7) == view(5), 'DOWN scrolls, UP scrolls back')
    check(view(8) != view(5), 'the bottom differs from the top')
    check(view(10) == view(8), 'UP from page 2 lands on the bottom of page 1')
    check(s[11]['depth'] == 0, 'q leaves')

    writing = check_writing(results, raw, annots, unchanged, saved, src, shot, check)

    print('PDF checks passed: %d checks' % checks)
    print(writing)
    print('open %.3f s, first page %.3f s, VmHWM %s kB (basic), %s kB (search)' % (float(info['OPEN']), first_tick, info['VMHWM'], info2['VMHWM']))
    print('re-renders: max %.3f s' % max(ticks[1:]))


if __name__ == '__main__':
    main()
