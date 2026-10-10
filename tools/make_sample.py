#!/usr/bin/env python3
"""Writes a small gradient PNG used as the subject of the screenshots.

Kept dependency free so continuous integration needs nothing but Python.
"""

import struct
import sys
import zlib


def write_png(path: str, width: int, height: int) -> None:
    rows = []
    for y in range(height):
        row = bytearray([0])  # filter byte: none
        for x in range(width):
            row += bytes((
                (x * 255) // width,
                (y * 255) // height,
                max(0, 200 - ((x + y) * 120) // (width + height)),
                255,
            ))
        rows.append(bytes(row))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", header)
           + chunk(b"IDAT", zlib.compress(b"".join(rows), 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as handle:
        handle.write(png)


if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else "sample.png"
    write_png(target, 640, 420)
    print("wrote", target)
