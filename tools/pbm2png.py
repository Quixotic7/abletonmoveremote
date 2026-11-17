#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""A P1 PBM of the Move's screen (the test hook's mr-screen.pbm) to a PNG, scaled up, white on black like the OLED.

    pbm2png.py IN.pbm OUT.png [SCALE]
"""
import struct
import sys
import zlib


def main():
    src, dst = sys.argv[1], sys.argv[2]
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    with open(src) as f:
        lines = [l.strip() for l in f if l.strip()]
    w, h = (int(v) for v in lines[1].split())
    raw = b""
    for r in lines[2:2 + h]:
        row = bytes(255 if c == "1" else 0 for c in r[:w] for _ in range(scale))
        raw += (b"\x00" + row) * scale

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 0, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    with open(dst, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    main()
