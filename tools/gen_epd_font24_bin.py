#!/usr/bin/env python3
"""从 ebook my_chinese_font.h 生成 epd_font24.bin，烧入 font_data 分区。

二进制布局（小端）：
  magic[8]     = b"EPD24FNT"
  version      u32 = 1
  char_size    u16 = 24
  glyph_bytes  u16 = 72
  num_chars    u32
  reserved[12] = 0          # 头共 32 字节
  glyphs[num_chars]:
      unicode  u16
      bitmap   u8[72]       # 共 74 字节/字，按 unicode 升序
"""

from __future__ import annotations

import argparse
import re
import struct
import pathlib

MAGIC = b"EPD24FNT"
HEADER_SIZE = 32
GLYPH_BYTES = 72
RECORD_SIZE = 2 + GLYPH_BYTES  # 74


def parse_glyphs(src: pathlib.Path) -> list[tuple[int, bytes]]:
    text = src.read_text(encoding="utf-8", errors="ignore")
    pat = re.compile(r"\{\s*(0x[0-9A-Fa-f]+)\s*,\s*\{([^}]+)\}\s*\}")
    glyphs: dict[int, bytes] = {}
    for m in pat.finditer(text):
        u = int(m.group(1), 16)
        raw = [int(b.strip(), 0) for b in m.group(2).split(",") if b.strip()]
        if len(raw) != GLYPH_BYTES:
            raise SystemExit(f"glyph {u:#x} len={len(raw)} want {GLYPH_BYTES}")
        glyphs[u] = bytes(raw)
    if not glyphs:
        raise SystemExit(f"no glyphs in {src}")
    return sorted(glyphs.items(), key=lambda kv: kv[0])


def write_bin(glyphs: list[tuple[int, bytes]], out: pathlib.Path) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("wb") as f:
        header = struct.pack(
            "<8sIHH I12s",
            MAGIC,
            1,
            24,
            GLYPH_BYTES,
            len(glyphs),
            b"\x00" * 12,
        )
        assert len(header) == HEADER_SIZE
        f.write(header)
        for u, bmp in glyphs:
            f.write(struct.pack("<H", u))
            f.write(bmp)
    size = out.stat().st_size
    expect = HEADER_SIZE + len(glyphs) * RECORD_SIZE
    if size != expect:
        raise SystemExit(f"size mismatch {size} != {expect}")
    print(f"wrote {out}  glyphs={len(glyphs)}  size={size} ({size/1024/1024:.2f} MiB)")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "-i",
        "--input",
        type=pathlib.Path,
        default=pathlib.Path(
            r"E:\code\470moshuiping\4.7-inch(684x1216)_ebook\main\common\my_chinese_font.h"
        ),
    )
    ap.add_argument(
        "-o",
        "--output",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1] / "assets" / "epd_font24.bin",
    )
    args = ap.parse_args()
    if not args.input.is_file():
        raise SystemExit(f"input not found: {args.input}")
    glyphs = parse_glyphs(args.input)
    write_bin(glyphs, args.output)


if __name__ == "__main__":
    main()
