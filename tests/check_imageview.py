#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Stage 2 image viewer checks (docs/FDTP_VIEWERS.md). Everything generated
stays under BUILD; no network. Builds the harness (tests/imageview_harness.c)
with make, runs it once plainly and once under ASan/UBSan (BUILD/asan)."""
import argparse
import importlib.util
import os
from pathlib import Path
import random
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('fixtures', ROOT / 'tools/make_test_images.py')
fixtures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixtures)
NAVY = (0, 0, 85)
SANITIZE = '-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined'


def require(value, message):
    if not value:
        raise AssertionError(message)


def ppm(path):
    data = path.read_bytes()
    magic, wh, maxv, pixels = data.split(b'\n', 3)
    require(magic == b'P6' and maxv == b'255', f'{path}: not P6')
    w, h = map(int, wh.split())
    require(len(pixels) == w * h * 3, f'{path}: length')
    return w, h, pixels


def pixel(im, x, y):
    w, h, data = im
    require(0 <= x < w and 0 <= y < h, f'pixel {x},{y} outside {w}x{h}')
    return tuple(data[(y * w + x) * 3:(y * w + x + 1) * 3])


def near(a, b, tolerance):
    return all(abs(p - q) <= tolerance for p, q in zip(a, b))


class Suite:
    def __init__(self, exe, out, env):
        self.exe, self.out, self.env, self.count = exe, out, env, 0

    def run(self, *parts, expected=0, timeout=60):
        r = subprocess.run([str(self.exe), *map(str, parts)], capture_output=True, text=True,
                           env=self.env, timeout=timeout)
        self.count += 1
        require('runtime error:' not in r.stderr and 'AddressSanitizer' not in r.stderr, (parts, r.stderr))
        require(r.returncode == expected, (parts, r.returncode, r.stdout, r.stderr))
        return r

    def decode(self, path, mw=1024, mh=552, expected=0):
        out = self.out / 'decode.ppm'
        r = self.run('--decode', path, mw, mh, out, expected=expected)
        return (ppm(out), r.stdout.split()) if not expected else (None, r.stderr)

    def view(self, path, keys, name):
        script = self.out / (name + '.keys')
        script.write_text(keys)
        prefix = self.out / name
        r = self.run(path, script, prefix)
        snaps = [line.split(' ', 3) for line in r.stdout.splitlines() if line.startswith('SNAP ')]
        images = [ppm(Path(f'{prefix}-{i + 1:03d}.ppm')) if int(snap[2]) else None for i, snap in enumerate(snaps)]
        notices = [s[3] if len(s) > 3 else '' for s in snaps]
        depths = [int(s[2]) for s in snaps]
        return images, notices, depths


def legacy_expected(name):
    """Indices and palette the fixture writer used, for an exact comparison."""
    pattern = fixtures.pattern_indices()
    colors16 = [((i * 37) % 256, (i * 91) % 256, (i * 53) % 256) for i in range(16)]
    quad = fixtures.quadrant_indices()
    table = {'mag16.mag': (quad, fixtures.COLORS, 1), 'mag256.mag': (quad, fixtures.COLORS, 1),
             'double.mag': (quad, fixtures.COLORS, 2), 'quadrants.pi': (quad, fixtures.COLORS, 1),
             'double.pi': (quad, fixtures.COLORS, 2), 'pattern16.mag': (pattern, colors16, 1),
             'pattern256.mag': (pattern, colors16, 1), 'pattern.pi': (pattern, colors16, 1)}
    return table.get(name)


def check_decoders(s, src):
    for path in sorted(src.iterdir()):
        if path.name == 'random.png':
            continue
        im, info = s.decode(path)
        legacy = legacy_expected(path.name)
        if legacy:
            indices, palette, repeat = legacy
            w, h = len(indices[0]), len(indices) * repeat
            require(im[:2] == (w, h), (path, im[:2]))
            for y in range(h):
                for x in range(w):
                    require(pixel(im, x, y) == tuple(palette[indices[y // repeat][x]]), (path, x, y, pixel(im, x, y)))
            require(info[-1] == ('256' if '256' in path.name else '16'), (path, info))
            continue
        large = path.name.startswith('large.')
        require(im[:2] == ((1024, 512) if large else (16, 16)), (path, im[:2]))
        if path.name.startswith('exif'):
            o = int(path.stem[4:])
            maps = {1: [0, 1, 2, 3], 2: [1, 0, 3, 2], 3: [3, 2, 1, 0], 4: [2, 3, 0, 1],
                    5: [0, 2, 1, 3], 6: [2, 0, 3, 1], 7: [3, 1, 2, 0], 8: [1, 3, 0, 2]}
            for j, (x, y) in enumerate(((2, 2), (12, 2), (2, 12), (12, 12))):
                require(near(pixel(im, x, y), fixtures.COLORS[maps[o][j]], 3), (o, j, pixel(im, x, y)))
            continue
        if path.name == 'alpha.png':
            # Left half alpha 128 over navy, right half alpha 0: navy.
            require(pixel(im, 2, 2) == (128, 0, 42) and pixel(im, 12, 2) == NAVY, 'PNG alpha over navy')
            continue
        if path.name == 'checker.png':
            require(pixel(im, 2, 2) == (0, 0, 0), 'checker')
            continue
        w, h = im[:2]
        tolerance = 25 if path.suffix in ('.jpg', '.webp') else 0
        for x, y, index in ((w // 8, h // 8, 0), (w - w // 8, h // 8, 1), (w // 8, h - h // 8, 2), (w - w // 8, h - h // 8, 3)):
            if path.name == 'bmp1.bmp':
                index = int(x >= w // 2)
            require(near(pixel(im, x, y), fixtures.COLORS[index], tolerance), (path, x, y, pixel(im, x, y)))
        if path.name not in ('large.png', 'large.jpg', 'bmp1.bmp'):
            small, _ = s.decode(path, 1, 1)
            require(small[:2] == (1, 1) and near(pixel(small, 0, 0), (128, 128, 128), 3), (path, 'area average'))
    small, _ = s.decode(src / 'checker.png', 1, 1)
    require(pixel(small, 0, 0) == (128, 128, 128), 'checker area average')
    # JPEG asks libjpeg for 1/2 (scale_denom) and then fits; still the right colors.
    im, _ = s.decode(src / 'large.jpg', 256, 256)
    require(im[:2] == (256, 128) and near(pixel(im, 32, 32), (255, 0, 0), 25), 'large JPEG reduced')


def check_screen(s, src):
    for path in sorted(src.iterdir()):
        if path.name == 'random.png':
            continue
        images, notices, depths = s.view(path, 'SNAP i SNAP 1 SNAP + SNAP RIGHT DOWN SNAP - 0 SNAP q', 'basic-' + path.stem)
        first = images[0]
        require(first[:2] == (1024, 600), 'canvas size')
        require(pixel(first, 0, 200) == NAVY or path.name.startswith('large.'), (path, 'navy margin'))
        require(pixel(first, 0, 5) == NAVY and pixel(first, 1023, 595) == NAVY, (path, 'band background'))
        _, info = s.decode(path)
        require(f'{info[-3]} x {info[-2]}' in notices[1], (path, notices[1]))
        require(info[0] in notices[1], (path, 'format in i', notices[1]))
        require(depths == [1] * 6, (path, depths))
    # Revision 1 item 29: the picture area is the body of the screen's frame,
    # 936 x 504 from (44, 24); its centre (512, 276).
    # 16x16 at zoom 1 is centred: (504, 268) is the image's (0, 0).
    images, _, _ = s.view(src / 'quadrants.png', 'SNAP + SNAP + + + + SNAP 0 SNAP - - - - SNAP', 'zoom')
    require(pixel(images[0], 506, 270) == (255, 0, 0) and pixel(images[0], 503, 270) == NAVY, 'fit, no enlarge')
    require(pixel(images[1], 500, 264) == (255, 0, 0) and pixel(images[1], 495, 264) == NAVY, 'x2 nearest')
    # 8x is the ceiling: 128x128, red top-left quadrant reaches (448, 212).
    require(pixel(images[2], 449, 229) == (255, 0, 0) and pixel(images[2], 447, 229) == NAVY, 'x8 ceiling')
    require(pixel(images[3], 506, 270) == (255, 0, 0) and pixel(images[3], 503, 270) == NAVY, '0 fits again')
    # 1/8 is the floor: 2x2, one dot per quadrant at (511..512, 275..276).
    require(pixel(images[4], 511, 275) == (255, 0, 0) and pixel(images[4], 512, 276) == (255, 255, 255), '1/8 floor')
    require(pixel(images[4], 510, 275) == NAVY, '1/8 floor size')
    # 2048x1024: fit is 50 %; 1 then + is 200 %, arrows pan by 64 dots.
    images, notices, _ = s.view(src / 'large.png', 'SNAP 1 + SNAP RIGHT DOWN SNAP 0 SNAP', 'pan')
    require(pixel(images[0], 100, 100) == (255, 0, 0) and pixel(images[0], 100, 30) == NAVY, 'large fit')
    require(pixel(images[1], 500, 256) == (255, 0, 0), 'x2 before pan')
    require(pixel(images[2], 500, 256) == (255, 255, 255), 'x2 after pan right and down')
    require(images[3][2] == images[0][2], '0 returns to the fitted view')
    # Animated GIF moves with time: 0.1 s frames, loops forever.
    images, _, _ = s.view(src / 'animated.gif', 'SNAP WAIT:0.15 SNAP WAIT:0.1 SNAP ESC', 'animation')
    for image, colour in zip(images, ((255, 0, 0), (0, 255, 0), (255, 0, 0))):
        require(pixel(image, 506, 270) == colour, ('animation', pixel(image, 506, 270)))
    images, _, _ = s.view(src / 'quadrants.gif', 'SNAP WAIT:5 SNAP', 'still-gif')
    require(images[0][2] == images[1][2], 'still GIF stays')
    # SPC / BS: next and previous image in the directory, by name.
    nav = s.out / 'nav'
    nav.mkdir(exist_ok=True)
    for old in nav.iterdir():
        old.unlink()
    fixtures.png(nav / 'a.png')
    fixtures.png(nav / 'b.png', alpha=True)
    (nav / 'c.txt').write_text('not an image')
    (nav / 'd.bmp').write_bytes(b'BM broken')
    images, notices, depths = s.view(nav / 'a.png', 'SNAP SPC SNAP BS SNAP BS SNAP SPC SPC SNAP', 'navigation')
    require(pixel(images[1], 506, 270) == (128, 0, 42), 'SPC next')
    require(pixel(images[2], 506, 270) == (255, 0, 0), 'BS previous')
    require('前の画像はありません' in notices[3] and depths[3] == 1, 'no previous')
    require(depths[4] == 0 and 'BMP' in notices[4], ('broken next returns to the list', notices[4]))
    # A broken file: a notice and no screen.
    r = s.run(nav / 'd.bmp', s.out / 'navigation.keys', s.out / 'broken', expected=3)
    require('BMP' in r.stderr, r.stderr)


def check_malformed(s, src):
    corrupt = s.out / 'corrupt.bin'
    rng = random.Random(250)
    for path in sorted(src.iterdir()):
        data = path.read_bytes()
        for n in sorted(set((0, 1, 2, 8, 12, 32, 60, len(data) // 3, len(data) // 2, len(data) - 1))):
            if n >= len(data):
                continue
            corrupt.write_bytes(data[:n])
            r = subprocess.run([str(s.exe), '--decode', str(corrupt), '1024', '552', str(s.out / 'bad.ppm')],
                               capture_output=True, text=True, env=s.env, timeout=60)
            s.count += 1
            require(r.returncode in (0, 3), (path, n, r.returncode, r.stderr))
            require('runtime error:' not in r.stderr and 'AddressSanitizer' not in r.stderr, (path, n, r.stderr))
        # Random byte changes after the signature: decoded or refused, never a crash.
        for _ in range(25):
            bad = bytearray(data)
            for _ in range(rng.randrange(1, 6)):
                bad[rng.randrange(min(len(bad), 12), len(bad))] = rng.randrange(256)
            corrupt.write_bytes(bad)
            r = subprocess.run([str(s.exe), '--decode', str(corrupt), '1024', '552', str(s.out / 'bad.ppm')],
                               capture_output=True, text=True, env=s.env, timeout=60)
            s.count += 1
            require(r.returncode in (0, 3), (path, 'mutation', r.returncode, r.stderr))
            require('runtime error:' not in r.stderr and 'AddressSanitizer' not in r.stderr, (path, r.stderr))
    # Fractional 3x3 -> 2x2 area average (a dot and a half per side) and
    # padded 24-bit rows.
    row = bytes((0, 0, 255, 0, 255, 0, 255, 0, 0)) + b'\x00' * 3
    bmpdata = b'BM' + struct.pack('<IHHI', 90, 0, 0, 54) + struct.pack('<IiiHHIIiiII', 40, 3, 3, 1, 24, 0, 36, 0, 0, 0, 0) + row * 3
    corrupt.write_bytes(bmpdata)
    im, _ = s.decode(corrupt, 2, 2)
    require(im[:2] == (2, 2) and pixel(im, 0, 0) == (170, 85, 0) and pixel(im, 1, 1) == (0, 85, 170), 'BMP area weights')
    # Bad height / pixel offset / depth are refused before pixel data.
    for offset, packed in ((22, struct.pack('<i', -2147483648)), (10, struct.pack('<I', 5000)), (28, struct.pack('<H', 16))):
        bad = bytearray(bmpdata)
        bad[offset:offset + len(packed)] = packed
        corrupt.write_bytes(bad)
        s.decode(corrupt, expected=3)
    # Huge declared sizes with no data: refused quickly, no large allocation.
    corrupt.write_bytes(b'BM' + struct.pack('<IHHI', 0, 0, 0, 54) + struct.pack('<IiiHHIIiiII', 40, 60000, 60000, 1, 24, 0, 0, 0, 0, 0, 0))
    s.decode(corrupt, expected=3)
    corrupt.write_bytes(b'Pi\x1a\x00\x00\x00\x00\x04FDTP\x00\x00\xff\xff\xff\xff' + bytes(48) + b'\xff' * 8)
    s.decode(corrupt, expected=3)
    header = struct.pack('<4B4H5I', 0, 0, 0, 0, 0, 0, 16383, 16383, 80, 81, 0, 81, 0)
    corrupt.write_bytes(b'MAKI02  big\x1a' + header + bytes(48) + b'\x00')
    s.decode(corrupt, expected=3)
    # V4 header with explicit alpha: 1x1 red at alpha 128 over navy.
    dib = bytearray(108)
    struct.pack_into('<IiiHHII', dib, 0, 108, 1, 1, 1, 32, 3, 4)
    struct.pack_into('<4I', dib, 40, 0xff0000, 0xff00, 0xff, 0xff000000)
    corrupt.write_bytes(b'BM' + struct.pack('<IHHI', 126, 0, 0, 122) + dib + b'\x00\x00\xff\x80')
    im, _ = s.decode(corrupt)
    require(pixel(im, 0, 0) == (128, 0, 42), 'BMP V4 alpha')
    # MAG by hand: literal first row, copy up second, horizontal repeat third.
    palette = b''.join(bytes((i, i, i)) for i in range(16))
    header = struct.pack('<4B4H5I', 0, 0, 0, 0, 0, 0, 7, 2, 80, 81, 2, 83, 4)
    corrupt.write_bytes(b'MAKI02  copy\x1a' + header + palette + b'\x60\x44\x55\x12\x34\x56\x78')
    im, _ = s.decode(corrupt)
    require([pixel(im, x, 2)[0] for x in range(8)] == [5, 6, 7, 8, 5, 6, 7, 8], 'MAG by hand')
    signatures = [b'BM', b'GIF89a', b'\x89PNG\r\n\x1a\n', b'MAKI02  ', b'Pi', b'\xff\xd8', b'RIFF\x00\x00\x00\x00WEBP']
    for i in range(70):
        corrupt.write_bytes(signatures[i % len(signatures)] + rng.randbytes(rng.randrange(1, 1024)))
        s.decode(corrupt, expected=3)


def suite(exe, out, env):
    out.mkdir(parents=True, exist_ok=True)
    s = Suite(exe, out, env)
    src = out / 'fixtures'
    if src.exists():
        for old in src.iterdir():
            old.unlink()
    fixtures.generate(src)
    # JPEG and WebP come from the linked libjpeg/libwebp encoders (sips has no WebP).
    s.run('--make-modern', src)
    check_decoders(s, src)
    check_screen(s, src)
    check_malformed(s, src)
    return s.count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--no-sanitize', action='store_true', help='skip the ASan/UBSan run')
    args = parser.parse_args()
    build = args.build.resolve()
    (build / 'tmp').mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, FDTP_LANGUAGE='ja', TMPDIR=str(build / 'tmp'), UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1',
               ASAN_OPTIONS='abort_on_error=0:detect_leaks=' + ('1' if sys.platform != 'darwin' else '0'))
    runs = [(build, 'CFLAGS=-Werror', None, 'image-check')]
    if not args.no_sanitize:
        runs.append((build / 'asan', 'CFLAGS=-Werror -O1 -g ' + SANITIZE, 'LDFLAGS=' + SANITIZE, 'image-check-asan'))
    try:
        for target, cflags, ldflags, name in runs:
            command = ['make', '-s', '-C', str(ROOT), 'all', 'imageview-harness', cflags, 'BUILD=' + str(target)]
            subprocess.run(command + ([ldflags] if ldflags else []), check=True, env=env, stdout=subprocess.DEVNULL)
            count = suite(target / 'imageview-harness', build / name, env)
            print(f'PASS {name}: {count} runs (decode, canvas, zoom, pan, i, animation, SPC/BS, malformed)')
    except (AssertionError, OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
