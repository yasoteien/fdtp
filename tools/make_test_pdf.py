#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Test PDFs for the stage 3 viewer (docs/FDTP_VIEWERS.md), standard library only.

    make_test_pdf.py OUTDIR

sample.pdf    3 pages of different sizes: shapes, Helvetica text, Japanese text
              in non-embedded MS-Mincho / MS-Gothic (Type0, UniJIS-UCS2-H), so
              mupdf has to substitute its built-in CJK font.
password.pdf  RC4 40-bit (standard handler R2) with user password "secret".
broken.pdf    a PDF header followed by garbage.
truncated.pdf sample.pdf cut at two thirds (no xref): mupdf repairs what it can.
The geometry constants below are what tests/check_pdf.py checks against.
"""
import hashlib
import struct
import sys
from pathlib import Path

# Page sizes (points) and the shapes on them, PDF coordinates (origin bottom left).
PAGES = [
    {'size': (595, 842), 'rect': (100, 600, 200, 142), 'rgb': (1, 0, 0)},   # A4 portrait, red
    {'size': (842, 595), 'rect': (500, 100, 250, 150), 'rgb': (0, 0, 1)},   # A4 landscape, blue
    {'size': (300, 200), 'rect': (20, 20, 100, 60), 'rgb': (0, 0.6, 0)},    # small, green
]
# Text on page 1: baseline origin and size. The search word "target" is in it.
LATIN = {'page': 0, 'pos': (72, 500), 'size': 24, 'text': 'Hello FDTP search target'}
LATIN2 = {'page': 1, 'pos': (72, 400), 'size': 24, 'text': 'second page target'}
MINCHO = {'page': 0, 'pos': (72, 400), 'size': 36, 'text': '日本語の明朝体'}
GOTHIC = {'page': 0, 'pos': (72, 330), 'size': 36, 'text': 'ゴシック体の文字'}
PAD = bytes.fromhex('28BF4E5E4E758A4164004E56FFFA01082E2E00B6D0683E802F0CA9FE6453697A')


def rc4(key, data):
    s = list(range(256)); j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 255; s[i], s[j] = s[j], s[i]
    out = bytearray(); i = j = 0
    for b in data:
        i = (i + 1) & 255; j = (j + s[i]) & 255; s[i], s[j] = s[j], s[i]
        out.append(b ^ s[(s[i] + s[j]) & 255])
    return bytes(out)


def ucs2(text):
    return '<' + text.encode('utf-16-be').hex().upper() + '>'


def content(index):
    p = PAGES[index]; x, y, w, h = p['rect']
    ops = ['%.3g %.3g %.3g rg %d %d %d %d re f' % (*p['rgb'], x, y, w, h)]
    ops.append('0 0 0 RG 2 w 40 40 m %d %d l S' % (p['size'][0] - 40, 60))   # a line
    for t in (LATIN, LATIN2):
        if t['page'] == index:
            ops.append('0 g BT /F1 %d Tf %d %d Td (%s) Tj ET' % (t['size'], *t['pos'], t['text']))
    for font, t in (('F2', MINCHO), ('F3', GOTHIC)):
        if t['page'] == index:
            ops.append('0 g BT /%s %d Tf %d %d Td %s Tj ET' % (font, t['size'], *t['pos'], ucs2(t['text'])))
    return '\n'.join(ops).encode('ascii')


def cjk_font(objs, name, serif):
    desc = add(objs, b'<< /Type /FontDescriptor /FontName /%s /Flags %d /FontBBox [0 -141 1000 859] '
               b'/ItalicAngle 0 /Ascent 859 /Descent -141 /CapHeight 769 /StemV 78 >>' % (name, 6 if serif else 4))
    cid = add(objs, b'<< /Type /Font /Subtype /CIDFontType0 /BaseFont /%s /CIDSystemInfo << /Registry (Adobe) '
              b'/Ordering (Japan1) /Supplement 2 >> /FontDescriptor %d 0 R /DW 1000 >>' % (name, desc))
    return add(objs, b'<< /Type /Font /Subtype /Type0 /BaseFont /%s /Encoding /UniJIS-UCS2-H '
               b'/DescendantFonts [%d 0 R] >>' % (name, cid))


def add(objs, body):
    objs.append(body); return len(objs)


def stream(data, num, key):
    if key:
        k = hashlib.md5(key + struct.pack('<I', num)[:3] + b'\0\0').digest()[:min(16, len(key) + 5)]
        data = rc4(k, data)
    return b'<< /Length %d >>\nstream\n' % len(data) + data + b'\nendstream'


def build(pages, key=None, encrypt=None, file_id=b'\x11' * 16):
    objs = []
    f1 = add(objs, b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>')
    f2 = cjk_font(objs, b'MS-Mincho', True); f3 = cjk_font(objs, b'MS-Gothic', False)
    pages_num = len(objs) + 1; objs.append(None)
    kids = []
    for i in pages:
        c = len(objs) + 1; objs.append(stream(content(i), c, key))
        w, h = PAGES[i]['size']
        kids.append(add(objs, b'<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %d %d] /Contents %d 0 R '
                        b'/Resources << /Font << /F1 %d 0 R /F2 %d 0 R /F3 %d 0 R >> >> >>' % (pages_num, w, h, c, f1, f2, f3)))
    objs[pages_num - 1] = b'<< /Type /Pages /Kids [%s] /Count %d >>' % (b' '.join(b'%d 0 R' % k for k in kids), len(kids))
    catalog = add(objs, b'<< /Type /Catalog /Pages %d 0 R >>' % pages_num)
    enc = add(objs, encrypt) if encrypt else None
    out = bytearray(b'%PDF-1.4\n%\xe2\xe3\xcf\xd3\n'); offsets = []
    for n, body in enumerate(objs, 1):
        offsets.append(len(out)); out += b'%d 0 obj\n' % n + body + b'\nendobj\n'
    xref = len(out)
    out += b'xref\n0 %d\n0000000000 65535 f \n' % (len(objs) + 1)
    for o in offsets: out += b'%010d 00000 n \n' % o
    trailer = b'/Size %d /Root %d 0 R /ID [<%s> <%s>]' % (len(objs) + 1, catalog, file_id.hex().encode(), file_id.hex().encode())
    if enc: trailer += b' /Encrypt %d 0 R' % enc
    out += b'trailer\n<< %s >>\nstartxref\n%d\n%%%%EOF\n' % (trailer, xref)
    return bytes(out)


def encrypted(user=b'secret', owner=b'owner'):
    """Standard security handler, V1 R2 (RC4 40-bit), PDF 1.7 section 7.6.3."""
    pad = lambda p: (p + PAD)[:32]
    perms = -44; file_id = b'\x22' * 16
    o = rc4(hashlib.md5(pad(owner)).digest()[:5], pad(user))
    key = hashlib.md5(pad(user) + o + struct.pack('<i', perms) + file_id).digest()[:5]
    u = rc4(key, PAD)
    enc = b'<< /Filter /Standard /V 1 /R 2 /O <%s> /U <%s> /P %d >>' % (o.hex().encode(), u.hex().encode(), perms)
    return build([0], key=key, encrypt=enc, file_id=file_id)


def generate(out):
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    sample = build([0, 1, 2]); (out / 'sample.pdf').write_bytes(sample)
    (out / 'truncated.pdf').write_bytes(sample[:len(sample) * 2 // 3])
    (out / 'password.pdf').write_bytes(encrypted())
    junk = bytes((i * 73 + 41) % 256 for i in range(4096))
    (out / 'broken.pdf').write_bytes(b'%PDF-1.4\n' + junk)
    return out


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    generate(sys.argv[1])
