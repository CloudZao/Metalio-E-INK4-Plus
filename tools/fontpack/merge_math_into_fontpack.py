#!/usr/bin/env python3
"""把 Latin Modern Math 子集合并进 EFNT fontpack（默认 18/28/36 @2）。

字号与 UI（25/30）错开，避免覆盖汉字包里的 ASCII。
设备端通过 fontpack_lv_font_get(18|28|36, 2) 取数学字体。

示例：
  .venv/bin/python merge_math_into_fontpack.py \\
    --base fonts/MiSans-Mixed/fonts_misans_25_30.fontpack \\
    -o fonts/MiSans-Mixed/fonts_misans_25_30.fontpack --verify
"""

from __future__ import annotations

import argparse
import struct
import time
from pathlib import Path

import freetype

from build_fontpack import (
    GLYPH_META_SIZE,
    GlyphItem,
    make_key,
    render_glyph,
    verify_fontpack,
    write_fontpack,
)

ROOT = Path(__file__).resolve().parent
DEFAULT_MATH_TTF = ROOT.parent / "ttf-fonts/LatinModernMath/latinmodern-math.otf"
DEFAULT_CHARS = ROOT / "charset_math.txt"
DEFAULT_SIZES = (18, 28, 36)


def load_fontpack_items(path: Path) -> list[GlyphItem]:
    data = path.read_bytes()
    magic, ver, _flags, n, index_off, _data_off, _ = struct.unpack_from(
        "<4sHHIQQI", data, 0
    )
    if magic != b"EFNT" or ver != 1:
        raise SystemExit(f"bad fontpack: {path} magic={magic!r} ver={ver}")
    items: list[GlyphItem] = []
    for i in range(n):
        key, doff, dsz, _ = struct.unpack_from("<QIHH", data, index_off + i * 16)
        cp = key >> 32
        size = (key >> 16) & 0xFFFF
        bpp = key & 0xFFFF
        w, h, ox, oy, adv = struct.unpack_from("<HHhhH", data, doff)
        bmp = data[doff + GLYPH_META_SIZE : doff + dsz]
        items.append(GlyphItem(cp, size, bpp, w, h, ox, oy, adv, bmp))
    return items


def load_math_codepoints(path: Path) -> list[int]:
    text = path.read_text(encoding="utf-8")
    cps: set[int] = set()
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        # 行内「#」后为注释
        if "#" in s:
            s = s.split("#", 1)[0]
        for ch in s:
            cps.add(ord(ch))
    # 保证空格
    cps.add(0x20)
    return sorted(cps)


def main() -> None:
    ap = argparse.ArgumentParser(description="Merge Latin Modern Math into EFNT fontpack")
    ap.add_argument("--base", type=Path, required=True, help="源 fontpack")
    ap.add_argument("--math-ttf", type=Path, default=DEFAULT_MATH_TTF)
    ap.add_argument("--chars-file", type=Path, default=DEFAULT_CHARS)
    ap.add_argument(
        "--sizes",
        default="18,28,36",
        help="数学字号列表（与 UI 25/30 错开）",
    )
    ap.add_argument("--bpp", type=int, default=2, choices=(1, 2, 4))
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--verify", action="store_true")
    args = ap.parse_args()

    if not args.base.is_file():
        raise SystemExit(f"base not found: {args.base}")
    if not args.math_ttf.is_file():
        raise SystemExit(f"math TTF/OTF not found: {args.math_ttf}")
    if not args.chars_file.is_file():
        raise SystemExit(f"charset not found: {args.chars_file}")

    sizes = [int(x) for x in args.sizes.split(",") if x.strip()]
    if not sizes:
        raise SystemExit("empty --sizes")

    print(f"load {args.base} …")
    t0 = time.time()
    items = load_fontpack_items(args.base)
    print(f"  base items={len(items)} ({time.time() - t0:.1f}s)")

    cps = load_math_codepoints(args.chars_file)
    print(f"math charset cps={len(cps)} sizes={sizes} bpp={args.bpp}")
    print(f"  ttf={args.math_ttf}")

    face = freetype.Face(str(args.math_ttf))
    existing = {make_key(it.codepoint, it.size, it.bpp) for it in items}
    added = 0
    skipped = 0
    missing = 0
    t1 = time.time()
    for size in sizes:
        face.set_pixel_sizes(0, size)
        for cp in cps:
            key = make_key(cp, size, args.bpp)
            if key in existing:
                skipped += 1
                continue
            g = render_glyph(face, cp, size, args.bpp)
            if g is None:
                missing += 1
                continue
            items.append(g)
            existing.add(key)
            added += 1
        print(f"  size={size}: running added={added}")

    print(
        f"merge math: +{added} skip_dup={skipped} missing_glyph={missing} "
        f"({time.time() - t1:.1f}s) total={len(items)}"
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_fontpack(items, args.output)
    if args.verify:
        # 抽检：π / ≈ / ⎛ / 斜体 n
        samples = [
            (0x03C0, sizes[1] if len(sizes) > 1 else sizes[0], args.bpp),
            (0x2248, sizes[1] if len(sizes) > 1 else sizes[0], args.bpp),
            (0x239B, sizes[1] if len(sizes) > 1 else sizes[0], args.bpp),
            (0x1D45B, sizes[1] if len(sizes) > 1 else sizes[0], args.bpp),  # 𝑛
            (0x4F60, 30, 2),  # 汉字仍在
        ]
        verify_fontpack(args.output, samples)
    print(f"wrote {args.output} ({args.output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
