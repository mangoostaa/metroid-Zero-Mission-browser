#!/usr/bin/env python3
"""Wrap a raw RGBA8888 buffer (row-major, byte order R,G,B,A) into a PNG.

Standard library only (zlib + struct) so it runs on a bare Python with no Pillow.
Usage: rgba2png.py <in.rgba> <out.png> <width> <height>
"""
import sys
import zlib
import struct


def main():
    if len(sys.argv) < 5:
        sys.exit("usage: rgba2png.py <in.rgba> <out.png> <width> <height>")
    src, dst, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])

    with open(src, "rb") as f:
        data = f.read()
    stride = w * 4
    if len(data) < stride * h:
        sys.exit(f"short file: {len(data)} bytes, need {stride * h}")

    # Prepend the per-row filter byte (0 = None) that PNG scanlines require.
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += data[y * stride:(y + 1) * stride]

    def chunk(typ, payload):
        body = typ + payload
        return (struct.pack(">I", len(payload)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xffffffff))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))  # 8-bit RGBA
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")

    with open(dst, "wb") as f:
        f.write(png)
    print(f"wrote {dst} ({w}x{h})")


if __name__ == "__main__":
    main()
