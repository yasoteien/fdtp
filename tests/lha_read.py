# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Reading LHa headers in the tests (revision 1 D): levels 0, 1 and 2.

lha_read(path) -> list of members (dicts) in archive order:
  path (decoded, '/'-separated; a link's "|target" removed), raw (the
  stored bytes, 0xFF as '/'), link, method, level, os, size,
  packed, mtime, mode, crc, encoding ('ascii', 'utf-8' or 'cp932'),
  header_crc_ok (level 2: the 0x00 extension matches), offset, length
  (header and data), header_size, padded (a level 2 header with one
  padding byte).
lha_contents(lha, path, members) -> {path: bytes} of the files, from
  `lha pq` (Lhasa prints the files one after another in archive order).
"""
import struct
import subprocess


def crc16(data, crc=0):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def _decode(raw):
    if all(b < 0x80 for b in raw):
        return raw.decode('ascii'), 'ascii'
    try:
        return raw.decode('utf-8'), 'utf-8'
    except UnicodeDecodeError:
        return raw.decode('cp932'), 'cp932'


def lha_read(path):
    data = open(path, 'rb').read()
    out = []
    off = 0
    while off < len(data) and data[off] != 0:
        h = data[off:off + 26]
        method = h[2:7].decode('latin-1')
        level = h[20]
        packed, size, stamp = struct.unpack('<III', h[7:19])
        name = directory = b''
        mode = None
        crc_ok = None
        padded = False
        if level in (0, 1):
            hsize = h[0] + 2
            head = data[off:off + hsize]
            n = head[21]
            name = head[22:22 + n]
            crc = struct.unpack('<H', head[22 + n:24 + n])[0]
            os_id = chr(head[24 + n]) if level == 1 else ''
            length = hsize + packed
            if level == 1:
                pos = off + hsize
                nxt = struct.unpack('<H', head[hsize - 2:hsize])[0]
                while nxt:
                    ext = data[pos:pos + nxt]
                    if ext[0] == 1:
                        name = ext[1:-2]
                    elif ext[0] == 2:
                        directory = ext[1:-2]
                    elif ext[0] == 0x50:
                        mode = struct.unpack('<H', ext[1:3])[0]
                    pos += nxt
                    nxt = struct.unpack('<H', ext[-2:])[0]
            header_size = hsize
        elif level == 2:
            hsize = struct.unpack('<H', h[0:2])[0]
            head = bytearray(data[off:off + hsize])
            crc = struct.unpack('<H', head[21:23])[0]
            os_id = chr(head[23])
            pos = 24
            nxt = struct.unpack('<H', head[24:26])[0]
            pos = 26
            crc_at = None
            while nxt:
                ext = head[pos:pos + nxt]
                kind = ext[0]
                if kind == 0:
                    crc_at = pos + 1
                elif kind == 1:
                    name = bytes(ext[1:-2])
                elif kind == 2:
                    directory = bytes(ext[1:-2])
                elif kind == 0x50:
                    mode = struct.unpack('<H', ext[1:3])[0]
                following = struct.unpack('<H', ext[-2:])[0]
                pos += nxt
                nxt = following
            padded = pos < hsize
            if crc_at is not None:
                stored = struct.unpack('<H', head[crc_at:crc_at + 2])[0]
                head[crc_at:crc_at + 2] = b'\0\0'
                crc_ok = crc16(bytes(head)) == stored
            length = hsize + packed
            header_size = hsize
        else:
            raise ValueError(f'level {level}')
        raw = directory.replace(b'\xff', b'/')
        if raw and name and not raw.endswith(b'/'):
            raw += b'/'
        raw = (raw + name).rstrip(b'/')
        link = None
        if method == '-lhd-' and b'|' in raw:
            raw, target = raw.split(b'|', 1)
            link = _decode(target)[0]
        text, encoding = _decode(raw)
        # DOS names (levels 0 and 1) use '\'; after decoding, so that a
        # Shift_JIS second byte 0x5C (表, ソ) is not taken for one.
        if level < 2:
            text = text.replace('\\', '/')
        out.append(dict(path=text, raw=raw, link=link, method=method, level=level, os=os_id, size=size,
                        packed=packed, mtime=stamp, mode=mode, crc=crc, encoding=encoding, header_crc_ok=crc_ok,
                        offset=off, length=length, header_size=header_size, padded=padded))
        off += length
    return out


def lha_contents(lha, path, members):
    printed = subprocess.run([lha, 'pq', str(path)], check=True, capture_output=True).stdout
    out = {}
    at = 0
    for m in members:
        if m['method'] == '-lhd-':
            continue
        out[m['path']] = printed[at:at + m['size']]
        at += m['size']
    if at != len(printed):
        raise ValueError(f'lha p printed {len(printed)} bytes, the headers say {at}')
    return out
