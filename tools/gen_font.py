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


# ---- Glyphs the Shinonome fonts lack (drawn here, same 8x16 grid and
# one-dot strokes as their lower case: x-height rows 5-13) ----------------
# IPA letters for pronunciations (the stock dictionaries, revision 1 §16).
IPA = {
    0x00E6: ['.##.##..', '#..#..#.', '...#..#.', '.######.', '#..#....', '#..#....', '#..#..#.', '#..#..#.', '.##.##..'],  # æ
    0x0251: ['..###.#.', '.#...##.', '#.....#.', '#.....#.', '#.....#.', '#.....#.', '#.....#.', '.#...##.', '..###.#.'],  # ɑ
    0x0254: ['..###...', '.#...#..', '......#.', '......#.', '......#.', '......#.', '......#.', '.#...#..', '..###...'],  # ɔ
    0x0259: ['..###...', '.#...#..', '......#.', '......#.', '#######.', '#.....#.', '#.....#.', '.#...#..', '..###...'],  # ə
    0x026A: ['.#####..', '...#....', '...#....', '...#....', '...#....', '...#....', '...#....', '...#....', '.#####..'],  # ɪ
    0x028A: ['##...##.', '.#...#..', '#.....#.', '#.....#.', '#.....#.', '#.....#.', '#.....#.', '.#...#..', '..###...'],  # ʊ
    0x028C: ['...#....', '...#....', '..#.#...', '..#.#...', '.#...#..', '.#...#..', '#.....#.', '#.....#.', '#.....#.'],  # ʌ
    0x0261: ['..###.#.', '.#...##.', '#.....#.', '#.....#.', '#.....#.', '#.....#.', '#.....#.', '.#...##.', '..###.#.',
             '......#.', '.#####..'],                                                                              # ɡ
    0x014B: ['.#.###..', '.##...#.', '.#....#.', '.#....#.', '.#....#.', '.#....#.', '.#....#.', '.#....#.', '.#....#.',
             '......#.', '....##..'],                                                                              # ŋ
    0x0292: ['######..', '....#...', '...#....', '..###...', '.....#..', '......#.', '......#.', '......#.', '#....#..',
             '.####...'],                                                                                          # ʒ
    0x02D0: ['', '..###...', '...#....', '', '', '', '...#....', '..###...', ''],                                    # ː
}
TALL = {   # rows 1-15 (ascenders, descenders)
    0x00F0: ['.#...#..', '..##....', '.#.#....', '....#...', '..####..', '.#...#..', '#.....#.', '#.....#.', '#.....#.',
             '#.....#.', '#.....#.', '.#...#..', '..###...'],                                                      # ð
    0x0283: ['....##..', '...#..#.', '...#....', '...#....', '...#....', '...#....', '...#....', '...#....', '...#....',
             '...#....', '...#....', '...#....', '...#....', '#..#....', '.##.....'],                              # ʃ
}
ACUTE, GRAVE = ['....#...', '...#....'], ['..#.....', '...#....']
# The vowels with a stress mark: base, mark -> the precomposed character
# when Unicode has one, else a code of our own in U+F700- (font24_compose).
VOWELS = 'aeiouæɑɔəɪʊʌ'
PRECOMPOSED = {('a', 0x301): 0xE1, ('e', 0x301): 0xE9, ('i', 0x301): 0xED, ('o', 0x301): 0xF3, ('u', 0x301): 0xFA,
               ('a', 0x300): 0xE0, ('e', 0x300): 0xE8, ('i', 0x300): 0xEC, ('o', 0x300): 0xF2, ('u', 0x300): 0xF9,
               ('æ', 0x301): 0x1FD}
# Shift_JIS read as Windows CP932 gives these; the JIS fonts have the others.
CP932_ALIASES = {0xFF5E: 0x301C, 0x2225: 0x2016, 0xFF0D: 0x2212, 0xFFE0: 0x00A2, 0xFFE1: 0x00A3, 0xFFE2: 0x00AC,
                 0x2015: 0x2014}


def bits(pattern):
    return sum(1 << (7 - x) for x, c in enumerate(pattern) if c == '#')


def circled(letter):
    """Ⓒ Ⓤ (the dictionaries' countable and uncountable marks): 16 dots wide."""
    rows = [0] * 16
    for y in range(16):
        for x in range(16):
            d = ((x - 7.5) ** 2 + (y - 7.5) ** 2) ** 0.5
            if abs(d - 6.8) < 0.55:
                rows[y] |= 1 << (15 - x)
    for i, pattern in enumerate(letter):
        for x, c in enumerate(pattern):
            if c == '#':
                rows[5 + i] |= 1 << (15 - (5 + x))
    return rows


def add_extra(glyphs):
    for code, pattern in IPA.items():
        rows = [0] * 16
        for i, line in enumerate(pattern):
            rows[5 + i] = bits(line)
        glyphs[code] = (8, rows)
    for code, pattern in TALL.items():
        rows = [0] * 16
        for i, line in enumerate(pattern):
            rows[1 + i] = bits(line)
        glyphs[code] = (8, rows)
    glyphs[0x24B8] = (16, circled(['.###.', '#...#', '#....', '#....', '#...#', '.###.']))
    glyphs[0x24CA] = (16, circled(['#...#', '#...#', '#...#', '#...#', '#...#', '.###.']))
    compose = {}
    own = 0xF700
    for base in VOWELS:
        for mark, accent in ((0x301, ACUTE), (0x300, GRAVE)):
            code = PRECOMPOSED.get((base, mark))
            if code is None:
                code, own = own, own + 1
            rows = list(glyphs[ord(base)][1])
            rows[0:5] = [0] * 5          # no dot over i
            rows[2], rows[3] = bits(accent[0]), bits(accent[1])
            glyphs[code] = (8, rows)
            compose[(ord(base), mark)] = code
    for new, old in CP932_ALIASES.items():
        if new not in glyphs and old in glyphs:
            glyphs[new] = glyphs[old]
    return compose


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
    compose = add_extra(glyphs)
    for required in '?〓A0あ漢':
        if ord(required) not in glyphs:
            raise ValueError(f'missing required glyph: {required}')
    return glyphs, compose


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
    glyphs, compose = build(args.fonts)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('w', encoding='ascii') as output:
        output.write('/* Generated by tools/gen_font.py; do not edit. */\n')
        output.write('static const Font24Glyph font24_glyphs[] = {\n')
        for code, (width, rows) in sorted(glyphs.items()):
            output.write('    {0x%Xu, %d, {%s}},\n' %
                         (code, width, ','.join('0x%04Xu' % row for row in rows)))
        output.write('};\n')
        output.write('/* base, combining mark (U+0300/U+0301) -> the glyph drawn for the pair */\n')
        output.write('static const struct { uint32_t base, mark, code; } font24_pairs[] = {\n')
        for (base, mark), code in sorted(compose.items()):
            output.write('    {0x%Xu, 0x%Xu, 0x%Xu},\n' % (base, mark, code))
        output.write('};\n')
    if args.dump:
        for char in 'A0あ漢':
            width, rows = glyphs[ord(char)]
            print(char)
            for row in rows:
                print(''.join('#' if row & (1 << (width-1-x)) else '.' for x in range(width)))


if __name__ == '__main__':
    main()
