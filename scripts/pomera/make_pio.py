#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Make the Wi-Fi PIO version of the Pomera DM250's kernel device tree.

U-Boot on the Debian 13 image for the DM250 (the D250 image of
ekesete.net) reads the kernel's device tree from a Rockchip resource image
at 24 MiB into the SD. This reads that 1 MiB (saved to a file by
pomera_dma_pio.sh) and turns the "dmas" and "dma-names" properties of the
SDIO Wi-Fi host (/mmc@10218000) into FDT_NOP tokens. The sizes do not
change, so the resource image's header stays valid; dw_mmc then finds no
DMA channel for the Wi-Fi and uses PIO. The SD and the eMMC keep the DMA.

It only works on the exact 1 MiB of that image: the input must have the
sha256 ORIG and the result the sha256 WIFI, or nothing is written.

    python3 make_pio.py IN OUT

Exit status: 0 OUT written; 2 usage; 3 IN is not the known original;
4 the result is not the known Wi-Fi PIO version (OUT not written);
5 IN is not the expected structure. --expect-in and --expect-out replace
ORIG and WIFI (for the tests only).
"""
import argparse
import hashlib
import os
import struct
import sys

ORIG = "bc0798fb393803cb4368134f121aa6f47ae4d541eb0b85ff03a7d7b6f6770573"
WIFI = "1a73651d8b786cf347d69e1ec20f661e8b13c396de6de92672bcc7decc541fb2"
SIZE = 1 << 20          # the resource image region read from the SD
DTB_AT = 0x400          # rk-kernel.dtb inside it
TARGET = "/mmc@10218000"
NAMES = ("dmas", "dma-names")

FDT_MAGIC = 0xD00DFEED
BEGIN_NODE, END_NODE, PROP, NOP, END = 1, 2, 3, 4, 9


class Bad(Exception):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def walk(dtb):
    """Yields (token, offset, end, node path, property name) for the
    structure block of the flattened device tree in dtb."""
    if len(dtb) < 40:
        raise Bad("too short for a device tree header")
    magic, total, off_struct, off_strings, _, _, _, _, size_strings, size_struct = struct.unpack(">10I", dtb[:40])
    if magic != FDT_MAGIC:
        raise Bad("no device tree at 0x%x" % DTB_AT)
    if total > len(dtb) or off_struct + size_struct > total or off_strings + size_strings > total:
        raise Bad("device tree header out of range")
    strings = bytes(dtb[off_strings:off_strings + size_strings])
    path = []
    i, stop = off_struct, off_struct + size_struct
    while i < stop:
        token = struct.unpack(">I", dtb[i:i + 4])[0]
        if token == BEGIN_NODE:
            e = dtb.index(b"\0", i + 4)
            path.append(bytes(dtb[i + 4:e]).decode("ascii", "replace"))
            end = (e + 1 + 3) & ~3
            yield token, i, end, "/".join(path) or "/", None
        elif token == END_NODE:
            if not path:
                raise Bad("unbalanced nodes")
            path.pop()
            end = i + 4
        elif token == PROP:
            length, name_off = struct.unpack(">II", dtb[i + 4:i + 12])
            end = (i + 12 + length + 3) & ~3
            if end > stop or name_off >= len(strings):
                raise Bad("property out of range")
            name = strings[name_off:strings.index(b"\0", name_off)].decode("ascii", "replace")
            yield token, i, end, "/".join(path) or "/", name
        elif token == NOP:
            end = i + 4
        elif token == END:
            return
        else:
            raise Bad("unknown token %d at 0x%x" % (token, i))
        i = end
    raise Bad("no end token")


def dtb_of(region):
    if len(region) < DTB_AT + 40:
        raise Bad("region too short")
    total = struct.unpack(">I", region[DTB_AT + 4:DTB_AT + 8])[0]
    if DTB_AT + total > len(region):
        raise Bad("device tree runs past the region")
    return region[DTB_AT:DTB_AT + total]


def wifi_pio(region):
    """Returns a copy of region with TARGET's dmas and dma-names turned
    into NOP tokens."""
    out = bytearray(region)
    dtb = bytearray(dtb_of(out))
    hits = []
    for token, start, end, node, name in walk(dtb):
        if token == PROP and node == TARGET and name in NAMES:
            hits.append(name)
            dtb[start:end] = struct.pack(">I", NOP) * ((end - start) // 4)
    if sorted(hits) != sorted(NAMES):
        raise Bad("%s has %s, not exactly %s" % (TARGET, hits or "none of them", " and ".join(NAMES)))
    out[DTB_AT:DTB_AT + len(dtb)] = dtb
    left = [(node, name) for token, _, _, node, name in walk(dtb_of(out))
            if token == PROP and node == TARGET and name in NAMES]
    if left or len(out) != len(region):
        raise Bad("the change did not take")
    return bytes(out)


def main(argv=None):
    p = argparse.ArgumentParser(description="Make the Wi-Fi PIO version of the DM250's resource image (1 MiB).")
    p.add_argument("input")
    p.add_argument("output")
    p.add_argument("--expect-in", default=ORIG, help=argparse.SUPPRESS)
    p.add_argument("--expect-out", default=WIFI, help=argparse.SUPPRESS)
    a = p.parse_args(argv)
    with open(a.input, "rb") as f:
        region = f.read()
    if len(region) != SIZE:
        print("make_pio: %s is %d bytes, not %d: stop" % (a.input, len(region), SIZE), file=sys.stderr)
        return 3
    if sha256(region) != a.expect_in:
        print("make_pio: %s is not the known original (sha256 %s): stop" % (a.input, sha256(region)), file=sys.stderr)
        return 3
    try:
        result = wifi_pio(region)
    except Bad as e:
        print("make_pio: %s: stop" % e, file=sys.stderr)
        return 5
    if sha256(result) != a.expect_out:
        print("make_pio: the result is not the known Wi-Fi PIO version (sha256 %s): stop, nothing written" % sha256(result),
              file=sys.stderr)
        return 4
    tmp = a.output + ".tmp"
    with open(tmp, "wb") as f:
        f.write(result)
    os.replace(tmp, a.output)
    changed = sum(x != y for x, y in zip(region, result))
    print("make_pio: %s written (%s's %s as NOP, %d bytes changed, sha256 %s)"
          % (a.output, TARGET, " and ".join(NAMES), changed, a.expect_out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
