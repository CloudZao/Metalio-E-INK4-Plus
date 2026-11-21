#!/usr/bin/env python3
"""Decode exact native Plus 4bpp bytes to a logical portrait PGM.

Use a buffer produced by the application renderer (host replay or explicit
device debug export), not a second approximation reconstructed from HTML.
"""
import argparse
from pathlib import Path


def decode(data):
    if len(data) != 1216 * 684 // 2:
        raise ValueError("Native 1216x684 4bpp buffer must be exactly 415872 bytes")
    out = bytearray(684 * 1216)
    for y in range(1216):
        for x in range(684):
            px, py = y, 683 - x
            packed = data[py * 608 + px // 2]
            gray = (packed >> (4 if px & 1 else 0)) & 15
            out[y * 684 + x] = gray * 17
    return bytes(out)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("frame", type=Path)
    p.add_argument("output", type=Path)
    a = p.parse_args()
    if a.frame.resolve() == a.output.resolve():
        raise ValueError("Output cannot overwrite source frame")
    pixels = decode(a.frame.read_bytes())
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(b"P5\n684 1216\n255\n" + pixels)
    print("NATIVE_FRAME_DECODED; source pixels only; physical panel not verified")


if __name__ == "__main__":
    main()
