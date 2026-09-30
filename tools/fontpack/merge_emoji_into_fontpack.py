#!/usr/bin/env python3
"""把 Noto Emoji 点阵合并进已有 EFNT fontpack（默认 30@2）。

示例：
  .venv/bin/python merge_emoji_into_fontpack.py \\
    --base fonts/MiSans-Mixed/.build_base.fontpack \\
    -o fonts/MiSans-Mixed/fonts_misans_25_30.fontpack
"""

from __future__ import annotations

import argparse
import struct
import time
from pathlib import Path

from fontTools.ttLib import TTFont

from build_fontpack import (
    GLYPH_META_SIZE,
    GlyphItem,
    make_key,
    render_glyph,
    verify_fontpack,
    write_fontpack,
)

DEFAULT_EMOJI_TTF = (
    Path(__file__).resolve().parent.parent / "ttf-fonts/Noto_Emoji/static/NotoEmoji-Regular.ttf"
)
SKIP_CPS = {0x0, 0xD, 0x200D, 0xFE0F, 0x20E3}


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


def emoji_codepoints(ttf: Path) -> list[int]:
    font = TTFont(str(ttf))
    cps: set[int] = set()
    for table in font["cmap"].tables:
        cps.update(table.cmap.keys())
    return sorted(c for c in cps if c not in SKIP_CPS)


def main() -> None:
    ap = argparse.ArgumentParser(description="Merge Noto Emoji glyphs into EFNT fontpack")
    ap.add_argument(
        "--base",
        type=Path,
        default=Path("fonts/MiSans-Mixed/fonts_misans_25_30.fontpack"),
        help="源 fontpack",
    )
    ap.add_argument(
        "--emoji-ttf",
        type=Path,
        default=DEFAULT_EMOJI_TTF,
        help="Noto Emoji TTF（建议 static Regular）",
    )
    ap.add_argument("--size", type=int, default=30, help="emoji 字号")
    ap.add_argument("--bpp", type=int, default=2, choices=(1, 2, 4), help="emoji BPP")
    ap.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("fonts/MiSans-Mixed/fonts_misans_25_30.fontpack"),
        help="输出路径",
    )
    ap.add_argument("--verify", action="store_true", help="写完后抽检汉字/emoji")
    args = ap.parse_args()

    if not args.base.is_file():
        raise SystemExit(f"base not found: {args.base}")
    if not args.emoji_ttf.is_file():
        raise SystemExit(f"emoji TTF not found: {args.emoji_ttf}")

    try:
        import freetype
    except ImportError as e:
        raise SystemExit("需要 freetype-py（tools/fontpack/.venv）") from e

    print(f"load {args.base} …")
    t0 = time.time()
    items = load_fontpack_items(args.base)
    print(f"  base items={len(items)} ({time.time() - t0:.1f}s)")

    keys = {g.key for g in items}
    cps = emoji_codepoints(args.emoji_ttf)
    print(f"emoji candidates={len(cps)} from {args.emoji_ttf.name}")

    face = freetype.Face(str(args.emoji_ttf))
    face.set_pixel_sizes(0, args.size)
    added = dup = missing = 0
    t1 = time.time()
    for i, cp in enumerate(cps, 1):
        key = make_key(cp, args.size, args.bpp)
        if key in keys:
            dup += 1
            continue
        g = render_glyph(face, cp, args.size, args.bpp)
        if g is None:
            missing += 1
            continue
        items.append(g)
        keys.add(key)
        added += 1
        if i % 500 == 0 or i == len(cps):
            print(f"  render {i}/{len(cps)} added={added}", flush=True)

    print(
        f"emoji done in {time.time() - t1:.1f}s: "
        f"added={added} dup={dup} missing={missing}"
    )
    write_fontpack(items, args.output)

    if args.verify:
        samples = [
            (0x4E2D, 25, 2),
            (0x4E2D, 30, 2),
            (0x4E2D, 30, 4),
            (0x1F600, args.size, args.bpp),
            (0x2764, args.size, args.bpp),
        ]
        present = {g.key for g in items}
        samples = [s for s in samples if make_key(*s) in present]
        verify_fontpack(args.output, samples)

    base_sz = args.base.stat().st_size
    out_sz = args.output.stat().st_size
    print(
        f"DONE {base_sz / 1024 / 1024:.2f}MiB → {out_sz / 1024 / 1024:.2f}MiB "
        f"(+{(out_sz - base_sz) / 1024:.1f}KB) items={len(items)}"
    )


if __name__ == "__main__":
    main()
