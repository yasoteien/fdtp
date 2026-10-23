#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""PPM (P6, the snapshots) to PNG, with the standard library only.

  ppm2png.py IN.ppm [OUT.png]     OUT defaults to IN with .png
"""
from pathlib import Path
import struct
import sys
import zlib


def read_ppm(path):
    data = Path(path).read_bytes()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    pos += 1
    if parts[0] != b'P6' or parts[3] != b'255':
        raise ValueError(f'{path}: not a P6 PPM')
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos:pos + w * h * 3]


def write_png(path, w, h, rgb):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)
    rows = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b'')
    Path(path).write_bytes(png)


def convert(src, dst=None):
    w, h, rgb = read_ppm(src)
    dst = Path(dst) if dst else Path(src).with_suffix('.png')
    write_png(dst, w, h, rgb)
    return dst


if __name__ == '__main__':
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        sys.exit(2)
    print(convert(*sys.argv[1:]))
