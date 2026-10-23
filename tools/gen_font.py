#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Build the FDTP font: gen_font.py [--dump] [--fonts DIR] [--output FILE].

Defaults are fonts/ (data/fonts/ in the public copy, whichever is there)
and build/font24.inc, both next to this script's tools/.
--dump prints the 16-dot A, slashed 0, あ and 漢 as #/. pixels.
Only Python's standard library is required; no host font or locale is used.
"""
import argparse
import gzip
from pathlib import Path
import struct


class PCF:
    def __init__(self, path):
        self.data = gzip.decompress(path.read_bytes())
        if self.data[:4] != b'\x01fcp':
            raise ValueError(f'{path}: not a PCF font')
        self.tables = {}
        count = struct.unpack_from('<I', self.data, 4)[0]
        for i in range(count):
            kind, fmt, size, offset = struct.unpack_from('<IIII', self.data, 8 + 16*i)
            # X11 PCF accelerator TOC sizes can include unused trailing
            # padding beyond EOF. Each consumed field is bounds checked by
            # struct instead of requiring that padding to be present.
            if offset + 4 > len(self.data):
                raise ValueError('truncated PCF table')
            self.tables[kind] = self.data[offset:offset+size]
        self.metrics = self.read_metrics()
        self.bitmaps, self.bitmap_format = self.read_bitmaps()
        self.encodings = self.read_encodings()
        # PCF_ACCELERATORS: format, eight one-byte flags, fontAscent.
        accel = self.tables.get(0x100, self.tables.get(2))
        self.ascent = self.unpack(accel, 'i', 12)[0] if accel else 22

    @staticmethod
    def unpack(table, fields, offset):
        fmt = struct.unpack_from('<I', table)[0]
        return struct.unpack_from(('>' if fmt & 4 else '<') + fields, table, offset)

    def read_metrics(self):
        table = self.tables[4]
        fmt = struct.unpack_from('<I', table)[0]
        if fmt & 0x100:
            count = self.unpack(table, 'H', 4)[0]
            return [tuple(v - 128 for v in struct.unpack_from('5B', table, 6 + 5*i))
                    for i in range(count)]
        count = self.unpack(table, 'I', 4)[0]
        return [self.unpack(table, '5hH', 8 + 12*i)[:5] for i in range(count)]

    def read_bitmaps(self):
        table = self.tables[8]
        fmt = struct.unpack_from('<I', table)[0]
        count = self.unpack(table, 'I', 4)[0]
        if count != len(self.metrics):
            raise ValueError('bitmap/metric count mismatch')
        offsets = self.unpack(table, f'{count}I', 8)
        sizes = self.unpack(table, '4I', 8 + count*4)
        start = 24 + count*4
        size = sizes[fmt & 3]
        data = table[start:start+size]
        if len(data) != size or any(o > size for o in offsets):
            raise ValueError('truncated PCF bitmaps')
        # PCF byte order applies to scan units in the contiguous bitmap
        # payload, not independently to scanlines. A scan unit can span
        # short rows (or glyph boundaries), so normalize before slicing.
        if bool(fmt & 4) != bool(fmt & 8):
            unit = 1 << ((fmt >> 4) & 3)
            data = b''.join(data[i:i+unit][::-1] for i in range(0, size, unit))
        return [data[o:(offsets[i+1] if i+1 < count else size)]
                for i, o in enumerate(offsets)], fmt

    def read_encodings(self):
        table = self.tables[0x20]
        low, high, first, last, _default = self.unpack(table, '5H', 4)
        count = (high-low+1)*(last-first+1)
        indices = self.unpack(table, f'{count}H', 14)
        result = {}
        for row in range(first, last+1):
            for col in range(low, high+1):
                index = indices[(row-first)*(high-low+1)+col-low]
                if index != 0xffff:
                    if index >= len(self.metrics):
                        raise ValueError('invalid PCF glyph index')
                    result[row*256+col] = index
        return result

    def glyph(self, encoding, width):
        index = self.encodings.get(encoding)
        if index is None:
            return None
        left, right, _advance, ascent, descent = self.metrics[index]
        fmt = self.bitmap_format
        pad = 1 << (fmt & 3)
        stride = ((right-left+7)//8 + pad-1)//pad*pad
        data = self.bitmaps[index]
        if stride*(ascent+descent) > len(data):
            raise ValueError('truncated glyph bitmap')
        rows = [0]*24
        for y in range(ascent+descent):
            scan = data[y*stride:(y+1)*stride]
            out_y = self.ascent-ascent+y
            for x in range(right-left):
                bit = 7-x%8 if fmt & 8 else x%8
                out_x = left+x
                if 0 <= out_y < 24 and 0 <= out_x < width and scan[x//8] & (1 << bit):
                    rows[out_y] |= 1 << (width-1-out_x)
        return rows


def build(fonts):
    """16-dot Shinonome glyphs (PC-98 look). The renderer scales them 1.5x
    to 12x24 cells by area averaging, as the FD 3.13 photo shows."""
    ascii_ = PCF(fonts/'shnm8x16a.pcf.gz')
    roman = PCF(fonts/'shnm8x16r.pcf.gz')
    japanese = PCF(fonts/'shnmk16.pcf.gz')
    glyphs = {}
    for code in range(32, 127):
        rows = ascii_.glyph(code, 8)
        if rows is not None:
            glyphs[code] = (8, rows[:16])
    for code in range(0xff61, 0xffa0):
        encoded = chr(code).encode('euc_jp')
        rows = roman.glyph(encoded[1], 8)
        if rows is not None:
            glyphs[code] = (8, rows[:16])
    for encoding in japanese.encodings:
        try:
            char = bytes([(encoding >> 8) | 0x80, (encoding & 255) | 0x80]).decode('euc_jp')
        except UnicodeError:
            continue
        if len(char) == 1:
            glyphs[ord(char)] = (16, japanese.glyph(encoding, 16)[:16])
    # Slashed zero like the PC-98 font in the photo: rising stroke inside the oval.
    rows = glyphs[ord('0')][1]
    for y, x in ((4, 5), (5, 5), (6, 4), (7, 4), (8, 3), (9, 3), (10, 2)):
        rows[y] |= 1 << (7-x)
    for required in '?〓A0あ漢':
        if ord(required) not in glyphs:
            raise ValueError(f'missing required glyph: {required}')
    return glyphs


def fonts_dir(root):
    """data/fonts (the public copy) or fonts (the private tree) under root."""
    public = root/'data'/'fonts'
    return public if public.is_dir() else root/'fonts'


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fonts', type=Path, default=fonts_dir(root))
    parser.add_argument('--output', type=Path, default=root/'build/font24.inc')
    parser.add_argument('--dump', action='store_true')
    args = parser.parse_args()
    glyphs = build(args.fonts)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('w', encoding='ascii') as output:
        output.write('/* Generated by tools/gen_font.py; do not edit. */\n')
        output.write('static const Font24Glyph font24_glyphs[] = {\n')
        for code, (width, rows) in sorted(glyphs.items()):
            output.write('    {0x%Xu, %d, {%s}},\n' %
                         (code, width, ','.join('0x%04Xu' % row for row in rows)))
        output.write('};\n')
    if args.dump:
        for char in 'A0あ漢':
            width, rows = glyphs[ord(char)]
            print(char)
            for row in rows:
                print(''.join('#' if row & (1 << (width-1-x)) else '.' for x in range(width)))


if __name__ == '__main__':
    main()
