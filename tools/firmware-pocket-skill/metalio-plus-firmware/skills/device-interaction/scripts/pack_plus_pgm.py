#!/usr/bin/env python3
"""Encode final-size, opaque P5 grayscale pixels for the pinned Plus HAL.

No image design/resizing is performed. Outputs go outside the installed Skill.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

WIDTH, HEIGHT = 1216, 684
PIXELS = WIDTH * HEIGHT
SKILL_ROOT = Path(__file__).resolve().parents[3]


def read_pgm(data: bytes) -> tuple[int, int, bytes]:
    pos = 0
    tokens = []
    while len(tokens) < 4:
        while pos < len(data):
            if data[pos] in b" \t\r\n\v\f":
                pos += 1
            elif data[pos] == 35:
                end = data.find(b"\n", pos)
                if end < 0:
                    raise ValueError("Unterminated PGM header comment")
                pos = end + 1
            else:
                break
        start = pos
        while pos < len(data) and data[pos] not in b" \t\r\n\v\f#":
            pos += 1
        if pos == start:
            raise ValueError("Incomplete PGM header")
        tokens.append(data[start:pos])
    if tokens[0] != b"P5" or tokens[3] != b"255":
        raise ValueError("Use binary P5 with maxval 255, not P2 or 16-bit PGM")
    try:
        width, height = int(tokens[1]), int(tokens[2])
    except ValueError as exc:
        raise ValueError("Invalid PGM dimensions") from exc
    if (width, height) not in ((WIDTH, HEIGHT), (HEIGHT, WIDTH)):
        raise ValueError("Plus input must be 1216x684 or 684x1216; resize before encoding")
    if pos >= len(data) or data[pos] not in b" \t\r\n\v\f":
        raise ValueError("Missing single whitespace separator before raster")
    # Consume only the header separator, never strip binary pixel bytes.
    pos += 2 if data[pos:pos + 2] == b"\r\n" else 1
    raster = data[pos:]
    if len(raster) != width * height:
        raise ValueError("Raster length mismatch or trailing bytes")
    return width, height, raster


def encode(raster: bytes, orientation: str, pixel_format: str, threshold: int) -> tuple[bytes, bytes]:
    if len(raster) != PIXELS:
        raise ValueError("Invalid raster size")
    if orientation not in ("portrait", "landscape") or pixel_format not in ("native4", "mono1"):
        raise ValueError("Unsupported orientation or format")
    if not 1 <= threshold <= 255:
        raise ValueError("Threshold must be 1..255")
    logical_width = HEIGHT if orientation == "portrait" else WIDTH
    packed = bytearray(PIXELS // (2 if pixel_format == "native4" else 8))
    quantized = bytearray(PIXELS)
    for index, value in enumerate(raster):
        x, y = index % logical_width, index // logical_width
        px, py = (y, HEIGHT - 1 - x) if orientation == "portrait" else (x, y)
        position = py * WIDTH + px
        if pixel_format == "native4":
            code = (value * 15 + 127) // 255
            packed[position // 2] |= code << (4 * (px & 1))
            quantized[index] = code * 17
        else:
            black = value < threshold
            if black:
                packed[position // 8] |= 1 << (7 - (px & 7))
            quantized[index] = 0 if black else 255
    return bytes(packed), bytes(quantized)


def decode(packed: bytes, orientation: str, pixel_format: str) -> bytes:
    expected = PIXELS // (2 if pixel_format == "native4" else 8)
    if len(packed) != expected:
        raise ValueError("Packed resource length does not match format")
    logical_width = HEIGHT if orientation == "portrait" else WIDTH
    raster = bytearray(PIXELS)
    for py in range(HEIGHT):
        for px in range(WIDTH):
            position = py * WIDTH + px
            if pixel_format == "native4":
                value = ((packed[position // 2] >> (4 * (px & 1))) & 15) * 17
            else:
                value = 0 if packed[position // 8] & (1 << (7 - (px & 7))) else 255
            x, y = (HEIGHT - 1 - py, px) if orientation == "portrait" else (px, py)
            raster[y * logical_width + x] = value
    return bytes(raster)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--orientation", required=True, choices=("portrait", "landscape"))
    parser.add_argument("--format", required=True, choices=("native4", "mono1"))
    parser.add_argument("--threshold", type=int, help="Required for mono1; black when gray < threshold")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--force", action="store_true", help="Replace these three generated output files")
    args = parser.parse_args()
    try:
        if args.format == "mono1" and args.threshold is None:
            raise ValueError("mono1 requires an explicit --threshold (1..255)")
        if args.format == "native4" and args.threshold is not None:
            raise ValueError("native4 uses nearest 16-level quantization, not a mono threshold")
        source = args.input.resolve(strict=True)
        target = args.output.resolve()
        preview = target.with_name(target.name + ".preview.pgm")
        receipt = target.with_name(target.name + ".json")
        for output in (target, preview, receipt):
            if output == source or output.is_relative_to(SKILL_ROOT):
                raise ValueError("Outputs must be outside the Skill and cannot overwrite the input")
            if output.exists() and (not args.force or not output.is_file()):
                raise ValueError(f"Output already exists: {output}; use a fresh path or --force")
        original = source.read_bytes()
        width, height, raster = read_pgm(original)
        expected = (HEIGHT, WIDTH) if args.orientation == "portrait" else (WIDTH, HEIGHT)
        if (width, height) != expected:
            raise ValueError("Input dimensions do not match the declared orientation")
        packed, quantized = encode(raster, args.orientation, args.format, args.threshold or 1)
        decoded = decode(packed, args.orientation, args.format)
        if decoded != quantized:
            raise ValueError("Full pixel round-trip check failed")
        metadata = {
            "schema": "metalio-plus-screen-resource-v1",
            "format": args.format,
            "physical_width": WIDTH,
            "physical_height": HEIGHT,
            "logical_width": width,
            "logical_height": height,
            "orientation": args.orientation,
            "threshold": args.threshold,
            "bytes": len(packed),
            "source_sha256": hashlib.sha256(original).hexdigest(),
            "sha256": hashlib.sha256(packed).hexdigest(),
            "roundtrip_pixels": len(decoded),
            "roundtrip_passed": True,
            "hardware_display_tested": False,
        }
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(packed)
        preview.write_bytes(f"P5\n{width} {height}\n255\n".encode("ascii") + decoded)
        receipt.write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(metadata, ensure_ascii=False))
        return 0
    except (OSError, ValueError) as exc:
        parser.exit(2, f"ERROR: {exc}\n")


if __name__ == "__main__":
    raise SystemExit(main())
