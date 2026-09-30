#!/usr/bin/env python3
"""Convert fonts to epdfont (.ef) for ESP32 SD-card reading.

Formats:
  EPDFONT v1 — header + unicode intervals + glyph metrics + packed bitmaps.
  Device loads intervals/metrics into RAM; bitmaps are seek'd on demand.

Usage:
  # From existing LVGL fmt_txt .c (recommended for MiSans already in repo)
  python fontconvert.py --from-lvgl-c ../../main/display/font/font_misans_regular_25_2.c \\
      -o misans_25.ef

  # Optional: downsample 2bpp → 1bpp
  python fontconvert.py --from-lvgl-c font_misans_regular_25_2.c --bpp 1 -o misans_25_1bpp.ef

  # From TTF/OTF (needs freetype-py in .venv)
  .venv/bin/python fontconvert.py --from-ttf MiSans-Regular.ttf --size 25 --bpp 1 \\
      --intervals ascii,cjk_common -o misans_25.ef
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

EPDFONT_MAGIC = b"EPDFONT\x00"
EPDFONT_VERSION = 1
HEADER_SIZE = 64  # matches epdfont_header_t

INTERVAL_PRESETS: dict[str, list[tuple[int, int]]] = {
    "ascii": [(0x0020, 0x007E)],
    "latin1": [(0x00A0, 0x00FF)],
    "punctuation": [(0x2000, 0x206F), (0x3000, 0x303F), (0xFF00, 0xFFEF)],
    "cjk": [(0x4E00, 0x9FFF), (0x3400, 0x4DBF), (0xF900, 0xFAFF)],
    "cjk_common": [(0x4E00, 0x9FA5)],  # roughly GB-level CJK unified
    "cjk_punct": [(0x3000, 0x303F), (0xFF01, 0xFF5E)],
    "reading_zh": [
        (0x0020, 0x007E),
        (0x00A0, 0x00FF),
        (0x2000, 0x206F),
        (0x3000, 0x303F),
        (0x4E00, 0x9FA5),
        (0xFF00, 0xFFEF),
    ],
}


@dataclass
class Glyph:
    cp: int
    width: int
    height: int
    adv_w: int  # pixels
    ofs_x: int
    ofs_y: int
    bitmap: bytes  # packed bpp bitstream (LVGL plain, no row pad)


def merge_intervals(ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    if not ranges:
        return []
    sorted_r = sorted(ranges)
    out = [sorted_r[0]]
    for a, b in sorted_r[1:]:
        la, lb = out[-1]
        if a <= lb + 1:
            out[-1] = (la, max(lb, b))
        else:
            out.append((a, b))
    return out


def resolve_intervals(spec: str) -> list[tuple[int, int]]:
    parts = [p.strip() for p in spec.split(",") if p.strip()]
    ranges: list[tuple[int, int]] = []
    for p in parts:
        if p in INTERVAL_PRESETS:
            ranges.extend(INTERVAL_PRESETS[p])
            continue
        m = re.fullmatch(r"\(0x([0-9a-fA-F]+)-0x([0-9a-fA-F]+)\)", p)
        if m:
            a, b = int(m.group(1), 16), int(m.group(2), 16)
            if a <= b:
                ranges.append((a, b))
            continue
        raise SystemExit(f"Unknown interval preset/range: {p}")
    return merge_intervals(ranges)


def downsample_2_to_1(bitmap: bytes, w: int, h: int) -> bytes:
    """2bpp MSB-first → 1bpp MSB-first; any non-zero AA → ink."""
    out = bytearray((w * h + 7) // 8)
    bit_i = 0
    for y in range(h):
        for x in range(w):
            src_i = y * w + x
            byte = bitmap[src_i >> 2]
            shift = 6 - 2 * (src_i & 3)
            val = (byte >> shift) & 0x3
            if val:
                out[bit_i >> 3] |= 1 << (7 - (bit_i & 7))
            bit_i += 1
    return bytes(out)


def pack_glyphs_to_xzf(
    glyphs: list[Glyph],
    bpp: int,
    line_height: int,
    base_line: int,
    out_path: Path,
) -> None:
    if bpp not in (1, 2):
        raise ValueError("bpp must be 1 or 2")
    glyphs = sorted(glyphs, key=lambda g: g.cp)
    if not glyphs:
        raise SystemExit("no glyphs to write")

    # Build contiguous unicode intervals (only where consecutive cps present)
    intervals: list[tuple[int, int, int]] = []  # first, last, glyph_index
    start = glyphs[0].cp
    prev = glyphs[0].cp
    start_idx = 0
    for i in range(1, len(glyphs)):
        cp = glyphs[i].cp
        if cp == prev + 1:
            prev = cp
            continue
        intervals.append((start, prev, start_idx))
        start = cp
        prev = cp
        start_idx = i
    intervals.append((start, prev, start_idx))

    # Fill holes: epdfont intervals require EVERY codepoint in [first,last] to exist.
    # So we split into runs of consecutive cps only (already done above).

    bitmap_blob = bytearray()
    glyph_recs: list[tuple] = []
    for g in glyphs:
        bmp = g.bitmap
        if bpp == 1 and g.width and g.height:
            # Caller should already convert; trust bmp length
            pass
        off = len(bitmap_blob)
        bitmap_blob.extend(bmp)
        glyph_recs.append(
            (g.width, g.height, g.adv_w, g.ofs_x, g.ofs_y, len(bmp), off, 0)
        )

    intervals_off = HEADER_SIZE
    glyphs_off = intervals_off + 12 * len(intervals)
    bitmaps_off = glyphs_off + 16 * len(glyph_recs)

    flags = bpp & 0xF
    header = struct.pack(
        "<8sHHHHhhIIIII24s",
        EPDFONT_MAGIC,
        EPDFONT_VERSION,
        flags,
        line_height,
        base_line,
        line_height - base_line,  # ascender approx
        -base_line,  # descender approx
        len(intervals),
        len(glyph_recs),
        intervals_off,
        glyphs_off,
        bitmaps_off,
        b"\x00" * 24,
    )
    assert len(header) == HEADER_SIZE

    with out_path.open("wb") as f:
        f.write(header)
        for first, last, gidx in intervals:
            f.write(struct.pack("<III", first, last, gidx))
        for w, h, adv, ox, oy, blen, boff, _ in glyph_recs:
            f.write(struct.pack("<BBHhhHIH", w, h, adv, ox, oy, blen, boff, 0))
        f.write(bitmap_blob)

    print(
        f"Wrote {out_path}  glyphs={len(glyph_recs)} intervals={len(intervals)} "
        f"bpp={bpp} size={out_path.stat().st_size} bytes"
    )


# --------------- From LVGL .c ---------------

def parse_lvgl_c(path: Path) -> tuple[list[Glyph], int, int, int]:
    text = path.read_text(encoding="utf-8", errors="replace")

    m_lh = re.search(r"\.line_height\s*=\s*(\d+)", text)
    m_bl = re.search(r"\.base_line\s*=\s*(\d+)", text)
    m_bpp = re.search(r"\.bpp\s*=\s*(\d+)", text)
    if not (m_lh and m_bl and m_bpp):
        raise SystemExit("cannot find line_height/base_line/bpp in C font")
    line_height = int(m_lh.group(1))
    base_line = int(m_bl.group(1))
    bpp = int(m_bpp.group(1))

    # bitmap array
    m_bmp = re.search(
        r"static\s+LV_ATTRIBUTE_LARGE_CONST\s+const\s+uint8_t\s+glyph_bitmap\[\]\s*=\s*\{(.*?)\};",
        text,
        re.S,
    )
    if not m_bmp:
        m_bmp = re.search(
            r"static\s+const\s+uint8_t\s+glyph_bitmap\[\]\s*=\s*\{(.*?)\};",
            text,
            re.S,
        )
    if not m_bmp:
        raise SystemExit("glyph_bitmap[] not found")
    hex_bytes = re.findall(r"0x([0-9a-fA-F]{1,2})", m_bmp.group(1))
    bitmap = bytes(int(h, 16) for h in hex_bytes)
    print(f"  bitmap bytes: {len(bitmap)}")

    # glyph_dsc
    m_dsc = re.search(
        r"static\s+const\s+lv_font_fmt_txt_glyph_dsc_t\s+glyph_dsc\[\]\s*=\s*\{(.*?)\};",
        text,
        re.S,
    )
    if not m_dsc:
        raise SystemExit("glyph_dsc[] not found")
    dsc_entries = re.findall(
        r"\{[^}]*?\.bitmap_index\s*=\s*(\d+)\s*,\s*\.adv_w\s*=\s*(\d+)\s*,\s*"
        r"\.box_w\s*=\s*(\d+)\s*,\s*\.box_h\s*=\s*(\d+)\s*,\s*"
        r"\.ofs_x\s*=\s*(-?\d+)\s*,\s*\.ofs_y\s*=\s*(-?\d+)\s*\}",
        m_dsc.group(1),
    )
    if not dsc_entries:
        raise SystemExit("failed to parse glyph_dsc entries")
    print(f"  glyph_dsc count: {len(dsc_entries)} (incl. id0)")

    # Build cmap: unicode -> glyph_id
    # Parse FORMAT0_TINY and SPARSE_TINY cmaps
    cp_to_gid: dict[int, int] = {}

    # unicode_list_N arrays
    ulists: dict[int, list[int]] = {}
    for m in re.finditer(
        r"static\s+const\s+uint16_t\s+unicode_list_(\d+)\[\]\s*=\s*\{(.*?)\};",
        text,
        re.S,
    ):
        idx = int(m.group(1))
        vals = [int(x.strip()) for x in m.group(2).split(",") if x.strip()]
        ulists[idx] = vals

    # cmap structs — match both tiny formats
    cmap_pat = re.compile(
        r"\{\s*\.range_start\s*=\s*(\d+)\s*,\s*\.range_length\s*=\s*(\d+)\s*,\s*"
        r"\.glyph_id_start\s*=\s*(\d+)\s*,\s*"
        r"\.unicode_list\s*=\s*([^,]+)\s*,\s*\.glyph_id_ofs_list\s*=\s*([^,]+)\s*,\s*"
        r"\.list_length\s*=\s*(\d+)\s*,\s*\.type\s*=\s*([A-Z0-9_]+)\s*\}",
        re.S,
    )
    m_cmaps = re.search(
        r"static\s+const\s+lv_font_fmt_txt_cmap_t\s+cmaps\[\]\s*=\s*\{(.*)\};",
        text,
        re.S,
    )
    if not m_cmaps:
        raise SystemExit("cmaps[] not found")
    # truncate at font_dsc to avoid greed matching too far — use non-greedy already via .*? mid
    # Actually the (.*) is greedy to last }; — constrain by finding cmap_num area
    cmap_body = m_cmaps.group(1)
    # cut before ALL CUSTOM DATA if present
    cut = cmap_body.find("/*--------------------")
    if cut > 0:
        cmap_body = cmap_body[:cut]

    for m in cmap_pat.finditer(cmap_body):
        range_start = int(m.group(1))
        range_length = int(m.group(2))
        glyph_id_start = int(m.group(3))
        unicode_list_tok = m.group(4).strip()
        list_length = int(m.group(6))
        ctype = m.group(7)

        if "FORMAT0_TINY" in ctype:
            for rcp in range(range_length):
                cp_to_gid[range_start + rcp] = glyph_id_start + rcp
        elif "SPARSE_TINY" in ctype:
            ul_m = re.search(r"unicode_list_(\d+)", unicode_list_tok)
            if not ul_m:
                print(f"  warn: sparse cmap without list at {range_start}")
                continue
            ul = ulists.get(int(ul_m.group(1)), [])
            for i, rcp in enumerate(ul[:list_length]):
                cp_to_gid[range_start + rcp] = glyph_id_start + i
        else:
            print(f"  warn: unsupported cmap type {ctype}, skipped")

    print(f"  mapped codepoints: {len(cp_to_gid)}")

    glyphs: list[Glyph] = []
    for cp, gid in sorted(cp_to_gid.items()):
        if gid <= 0 or gid >= len(dsc_entries):
            continue
        bi, adv_w, box_w, box_h, ofs_x, ofs_y = map(int, dsc_entries[gid])
        # adv_w is 8.4 fixed → pixels
        adv_px = (adv_w + 8) >> 4
        # bitmap length: until next glyph's bitmap_index (or end)
        next_bi = len(bitmap)
        for ng in range(gid + 1, len(dsc_entries)):
            nbi = int(dsc_entries[ng][0])
            if nbi > bi or (int(dsc_entries[ng][2]) == 0 and int(dsc_entries[ng][3]) == 0 and nbi == bi):
                if nbi > bi:
                    next_bi = nbi
                    break
                continue
            if nbi > bi:
                next_bi = nbi
                break
        # Simpler: find next non-equal larger bitmap_index
        for ng in range(gid + 1, len(dsc_entries)):
            nbi = int(dsc_entries[ng][0])
            if nbi > bi:
                next_bi = nbi
                break
        bmp = bitmap[bi:next_bi] if box_w and box_h else b"\x00"
        if box_w == 0 or box_h == 0:
            bmp = b"\x00"
        glyphs.append(
            Glyph(
                cp=cp,
                width=box_w,
                height=box_h,
                adv_w=adv_px,
                ofs_x=ofs_x,
                ofs_y=ofs_y,
                bitmap=bmp,
            )
        )

    return glyphs, bpp, line_height, base_line


def convert_from_lvgl_c(path: Path, out: Path, target_bpp: int | None) -> None:
    print(f"Parsing {path} ...")
    glyphs, src_bpp, line_height, base_line = parse_lvgl_c(path)
    bpp = target_bpp or src_bpp
    if bpp not in (1, 2):
        raise SystemExit("target bpp must be 1 or 2")
    if bpp == 1 and src_bpp == 2:
        print("  downsampling 2bpp → 1bpp")
        for g in glyphs:
            if g.width and g.height and g.bitmap:
                g.bitmap = downsample_2_to_1(g.bitmap, g.width, g.height)
            else:
                g.bitmap = b"\x00"
    elif bpp != src_bpp:
        raise SystemExit(f"cannot convert bpp {src_bpp} → {bpp}")
    pack_glyphs_to_xzf(glyphs, bpp, line_height, base_line, out)


# --------------- From TTF ---------------

def convert_from_ttf(
    ttf: Path,
    size: int,
    bpp: int,
    intervals: list[tuple[int, int]],
    out: Path,
) -> None:
    try:
        import freetype
    except ImportError as e:
        raise SystemExit(
            "freetype-py required. Use: tools/epdfont/.venv/bin/pip install freetype-py"
        ) from e

    face = freetype.Face(str(ttf))
    face.set_pixel_sizes(0, size)
    glyphs: list[Glyph] = []

    def load_cp(cp: int) -> Glyph | None:
        # Prefer unicode charmap
        idx = face.get_char_index(cp)
        if idx == 0 and cp != 0:
            return None
        flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL
        if bpp == 1:
            flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO
        face.load_char(cp, flags)
        glyph = face.glyph
        bitmap = glyph.bitmap
        w, h = bitmap.width, bitmap.rows
        adv = (glyph.advance.x + 32) >> 6
        ox = glyph.bitmap_left
        # LVGL ofs_y: distance from baseline to bottom of bitmap
        # baseline_y = 0; top = bitmap_top; bottom = bitmap_top - h
        # ofs_y in fmt_txt is typically (bitmap_top - h) relative...?
        # From lv_font_conv / FreeType docs used by LVGL:
        # ofs_y = -(rows - bitmap_top)  OR bitmap_top - rows
        # Looking at MiSans '!': box_h=19, ofs_y=0 → sits on baseline bottom
        # Actually LVGL: ofs_y measured from baseline to bottom of box (can be negative for descenders)
        oy = glyph.bitmap_top - h

        if w == 0 or h == 0:
            return Glyph(cp, 0, 0, max(adv, 1), 0, 0, b"\x00")

        buf = bytearray(bitmap.buffer)
        if bpp == 1:
            # FreeType mono: MSB left, pitch may pad
            pitch = bitmap.pitch
            out_bits = bytearray((w * h + 7) // 8)
            bit_i = 0
            for y in range(h):
                row = buf[y * pitch : (y + 1) * pitch]
                for x in range(w):
                    if row[x >> 3] & (0x80 >> (x & 7)):
                        out_bits[bit_i >> 3] |= 1 << (7 - (bit_i & 7))
                    bit_i += 1
            packed = bytes(out_bits)
        else:
            # Gray 8-bit → 2bpp
            pitch = bitmap.pitch
            out_bits = bytearray((w * h + 3) // 4)
            bit_i = 0
            for y in range(h):
                row = buf[y * pitch : y * pitch + w]
                for x in range(w):
                    v = row[x]
                    q = 0 if v < 32 else 1 if v < 96 else 2 if v < 160 else 3
                    shift = 6 - 2 * (bit_i & 3)
                    out_bits[bit_i >> 2] |= (q & 3) << shift
                    bit_i += 1
            packed = bytes(out_bits)

        return Glyph(cp, w, h, adv, ox, oy, packed)

    for a, b in intervals:
        for cp in range(a, b + 1):
            g = load_cp(cp)
            if g is not None:
                glyphs.append(g)

    # Metrics similar to LVGL
    asc = face.size.ascender >> 6
    desc = -(face.size.descender >> 6)
    line_height = asc + desc
    base_line = desc
    pack_glyphs_to_xzf(glyphs, bpp, line_height, base_line, out)


def main() -> None:
    ap = argparse.ArgumentParser(description="Build .ef fonts for epdfont loader")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--from-lvgl-c", type=Path, help="LVGL fmt_txt .c font source")
    src.add_argument("--from-ttf", type=Path, help="TTF/OTF font file")
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--bpp", type=int, choices=[1, 2], default=None)
    ap.add_argument("--size", type=int, default=25, help="pixel size for TTF")
    ap.add_argument(
        "--intervals",
        type=str,
        default="reading_zh",
        help="comma presets/ranges for TTF mode",
    )
    args = ap.parse_args()

    if args.from_lvgl_c:
        convert_from_lvgl_c(args.from_lvgl_c, args.output, args.bpp)
    else:
        iv = resolve_intervals(args.intervals)
        bpp = args.bpp or 1
        convert_from_ttf(args.from_ttf, args.size, bpp, iv, args.output)


if __name__ == "__main__":
    main()
