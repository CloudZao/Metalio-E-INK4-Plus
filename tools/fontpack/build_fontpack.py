#!/usr/bin/env python3
"""TTF/OTF → 单文件 fonts.fontpack（多字号 × 多 BPP，索引按 Key 升序）。

二进制布局（小端）：
  Header 32B:
    magic[4]="EFNT", version:u16, flags:u16, total_items:u32,
    index_offset:u64, data_offset:u64, reserved:u32
  Index 16B/条（按 key 升序）:
    key:u64 = (unicode<<32)|(size<<16)|bpp,
    data_offset:u32（相对文件头绝对偏移）, data_size:u16, reserved:u16
  Glyph data:
    width:u16, height:u16, x_offset:i16, y_offset:i16, advance:u16,
    bitmap_data[]（MSB-first 紧凑打包，无行填充）

设备端对 Index 做文件二分查找（fseek），只把单字点阵读入临时 Buffer。

示例：
  .venv/bin/python build_fontpack.py \\
    --ttf /mnt/f/MiSans/ttf/MiSans-Regular.ttf \\
    --sizes 16,20,24,32 --bpps 1,2 \\
    --from-ef ../epdfont/misans/misans_25_1.ef \\
    -o fonts.fontpack --verify
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "epdfont"))
from fontconvert import resolve_intervals  # noqa: E402

FONTPACK_MAGIC = b"EFNT"
FONTPACK_VERSION = 1
HEADER_SIZE = 32
INDEX_ENTRY_SIZE = 16
GLYPH_META_SIZE = 10  # w,h,ox,oy,adv


@dataclass
class GlyphItem:
    codepoint: int
    size: int
    bpp: int
    width: int
    height: int
    x_offset: int
    y_offset: int
    advance: int
    bitmap: bytes

    @property
    def key(self) -> int:
        return make_key(self.codepoint, self.size, self.bpp)

    @property
    def data_size(self) -> int:
        return GLYPH_META_SIZE + len(self.bitmap)


def make_key(codepoint: int, size: int, bpp: int) -> int:
    """64-bit Key: [32bit Unicode | 16bit Size | 16bit BPP]."""
    if not (0 <= codepoint <= 0xFFFFFFFF):
        raise ValueError(f"codepoint out of range: {codepoint}")
    if not (0 <= size <= 0xFFFF):
        raise ValueError(f"size out of range: {size}")
    if not (0 <= bpp <= 0xFFFF):
        raise ValueError(f"bpp out of range: {bpp}")
    return (codepoint << 32) | (size << 16) | bpp


def parse_int_list(spec: str) -> list[int]:
    """解析 '16,20,24' 或 '16-32' 或混合 '16,20-24,32'。"""
    out: list[int] = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a_s, b_s = part.split("-", 1)
            a, b = int(a_s), int(b_s)
            if a > b:
                raise SystemExit(f"bad range: {part}")
            out.extend(range(a, b + 1))
        else:
            out.append(int(part))
    if not out:
        raise SystemExit(f"empty list: {spec!r}")
    return sorted(set(out))


def load_codepoints_from_ef(path: Path) -> list[int]:
    data = path.read_bytes()
    magic, ver, _flags, _lh, _bl, _asc, _desc, ic, _gc, io, _go, _bo = struct.unpack_from(
        "<8sHHHHhhIIIII", data, 0
    )
    if not magic.startswith(b"EPDFONT") or ver != 1:
        raise SystemExit(f"bad .ef: {path}")
    cps: list[int] = []
    for i in range(ic):
        first, last, _gidx = struct.unpack_from("<III", data, io + i * 12)
        cps.extend(range(first, last + 1))
    return sorted(set(cps))


def load_codepoints_from_chars_file(path: Path) -> list[int]:
    """UTF-8 文本：逐字收集码点（忽略空白行与 # 注释行）。"""
    text = path.read_text(encoding="utf-8")
    cps: set[int] = set()
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        for ch in s:
            if not ch.isspace():
                cps.add(ord(ch))
    return sorted(cps)


def pack_mono_msb(buf: bytearray, pitch: int, w: int, h: int) -> bytes:
    out = bytearray((w * h + 7) // 8)
    bit_i = 0
    for y in range(h):
        row = buf[y * pitch : (y + 1) * pitch]
        for x in range(w):
            if row[x >> 3] & (0x80 >> (x & 7)):
                out[bit_i >> 3] |= 1 << (7 - (bit_i & 7))
            bit_i += 1
    return bytes(out)


def pack_gray_to_bpp(buf: bytearray, pitch: int, w: int, h: int, bpp: int) -> bytes:
    """8-bit gray → 2/4 bpp，MSB-first，无行填充。"""
    if bpp == 2:
        out = bytearray((w * h + 3) // 4)
    elif bpp == 4:
        out = bytearray((w * h + 1) // 2)
    else:
        raise ValueError(f"pack_gray_to_bpp: bpp={bpp}")

    bit_i = 0
    for y in range(h):
        row = buf[y * pitch : y * pitch + w]
        for x in range(w):
            v = row[x]
            if bpp == 2:
                q = 0 if v < 32 else 1 if v < 96 else 2 if v < 160 else 3
                out[bit_i >> 2] |= (q & 3) << (6 - 2 * (bit_i & 3))
            else:
                q = min(v >> 4, 15)
                out[bit_i >> 1] |= (q & 15) << (4 - 4 * (bit_i & 1))
            bit_i += 1
    return bytes(out)


def render_glyph(face, cp: int, size: int, bpp: int) -> GlyphItem | None:
    import freetype

    load_cp = cp
    tab_mul = 1
    idx = face.get_char_index(cp)
    if idx == 0 and cp != 0:
        if cp == 0x09:
            load_cp = 0x20
            tab_mul = 2
        else:
            return None

    if bpp == 1:
        flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO
    else:
        flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL

    face.load_char(load_cp, flags)
    g = face.glyph
    bm = g.bitmap
    w, h = bm.width, bm.rows
    adv = ((g.advance.x + 32) >> 6) * tab_mul
    ox = g.bitmap_left
    oy = g.bitmap_top - h  # 基线到盒底（与 LVGL/epdfont 一致）

    if w > 0xFFFF or h > 0xFFFF:
        print(f"  skip oversize U+{cp:04X} {w}x{h} @ {size}px", flush=True)
        return None

    if w == 0 or h == 0:
        return GlyphItem(cp, size, bpp, 0, 0, 0, 0, max(adv, 1), b"")

    buf = bytearray(bm.buffer)
    if bpp == 1:
        packed = pack_mono_msb(buf, bm.pitch, w, h)
    elif bpp in (2, 4):
        packed = pack_gray_to_bpp(buf, bm.pitch, w, h, bpp)
    else:
        raise ValueError(f"unsupported bpp: {bpp}")

    return GlyphItem(cp, size, bpp, w, h, ox, oy, adv, packed)


def parse_variants(spec: str) -> list[tuple[int, int]]:
    """解析 '25:1,30:1,30:2' → [(25,1),(30,1),(30,2)]。"""
    out: list[tuple[int, int]] = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if ":" not in part:
            raise SystemExit(f"bad variant (need size:bpp): {part!r}")
        size_s, bpp_s = part.split(":", 1)
        size, bpp = int(size_s), int(bpp_s)
        if bpp not in (1, 2, 4):
            raise SystemExit(f"bpp must be 1, 2 or 4; got {bpp}")
        out.append((size, bpp))
    if not out:
        raise SystemExit(f"empty variants: {spec!r}")
    return out


def parse_variant_map(spec: str) -> list[tuple[int, int, Path]]:
    """解析 '25:2=/path/a.ttf,30:4=/path/b.ttf' → [(25,2,Path),(30,4,Path)]。

    用 '=' 分隔规格与路径，避免 Windows/WSL 盘符冒号冲突。
    """
    out: list[tuple[int, int, Path]] = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "=" not in part:
            raise SystemExit(
                f"bad variant-map entry (need size:bpp=/path/to.ttf): {part!r}"
            )
        key_s, path_s = part.split("=", 1)
        key_s, path_s = key_s.strip(), path_s.strip()
        if ":" not in key_s:
            raise SystemExit(f"bad variant-map key (need size:bpp): {key_s!r}")
        size_s, bpp_s = key_s.split(":", 1)
        size, bpp = int(size_s), int(bpp_s)
        if bpp not in (1, 2, 4):
            raise SystemExit(f"bpp must be 1, 2 or 4; got {bpp}")
        ttf = Path(path_s)
        if not ttf.is_file():
            raise SystemExit(f"TTF not found for {size}:{bpp}: {ttf}")
        out.append((size, bpp, ttf))
    if not out:
        raise SystemExit(f"empty variant-map: {spec!r}")
    return out


def build_items(
    ttf: Path | None,
    codepoints: list[int],
    sizes: list[int],
    bpps: list[int],
    variants: list[tuple[int, int]] | None = None,
    variant_map: list[tuple[int, int, Path]] | None = None,
) -> list[GlyphItem]:
    try:
        import freetype
    except ImportError as e:
        raise SystemExit(
            "需要 freetype-py。请先: tools/fontpack/.venv/bin/pip install -r requirements.txt"
        ) from e

    if variant_map:
        jobs: list[tuple[int, int, Path]] = list(variant_map)
    else:
        if ttf is None:
            raise SystemExit("需要 --ttf，或使用 --variant-map")
        pairs = variants if variants else [(s, b) for s in sizes for b in bpps]
        jobs = [(s, b, ttf) for s, b in pairs]

    items: list[GlyphItem] = []
    total = len(codepoints) * len(jobs)
    done = 0
    t0 = time.time()

    # 按 TTF 分组，避免反复 open
    by_ttf: dict[Path, list[tuple[int, int]]] = {}
    for size, bpp, path in jobs:
        by_ttf.setdefault(path, []).append((size, bpp))

    for path, pairs in by_ttf.items():
        face = freetype.Face(str(path))
        print(f"  face={path.name}", flush=True)
        for size, bpp in pairs:
            face.set_pixel_sizes(0, size)
            print(f"  start size={size} bpp={bpp} ttf={path.name} …", flush=True)
            for cp in codepoints:
                g = render_glyph(face, cp, size, bpp)
                if g is not None:
                    items.append(g)
                done += 1
                if done % 2000 == 0 or done == total:
                    elapsed = time.time() - t0
                    print(
                        f"  render {done}/{total} ({100.0 * done / total:.1f}%) "
                        f"size={size} bpp={bpp} items={len(items)} {elapsed:.1f}s",
                        flush=True,
                    )
    return items


def write_fontpack(items: list[GlyphItem], out_path: Path) -> None:
    items.sort(key=lambda g: g.key)
    for i in range(1, len(items)):
        if items[i].key == items[i - 1].key:
            a = items[i]
            raise SystemExit(
                f"duplicate key U+{a.codepoint:04X} size={a.size} bpp={a.bpp}"
            )

    n = len(items)
    index_offset = HEADER_SIZE
    data_offset = HEADER_SIZE + n * INDEX_ENTRY_SIZE

    offsets: list[int] = []
    cur = data_offset
    for g in items:
        ds = g.data_size
        if ds > 0xFFFF:
            raise SystemExit(
                f"glyph data too large ({ds}) U+{g.codepoint:04X} "
                f"size={g.size} bpp={g.bpp}"
            )
        if cur > 0xFFFFFFFF:
            raise SystemExit("fontpack exceeds 4GiB (index data_offset is uint32)")
        offsets.append(cur)
        cur += ds

    header = struct.pack(
        "<4sHHIQQI",
        FONTPACK_MAGIC,
        FONTPACK_VERSION,
        0,  # flags
        n,
        index_offset,
        data_offset,
        0,  # reserved → pad Header to 32 bytes
    )
    assert len(header) == HEADER_SIZE

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("wb") as f:
        f.write(header)
        for g, off in zip(items, offsets):
            # key:u64, data_offset:u32, data_size:u16, reserved:u16
            f.write(struct.pack("<QIHH", g.key, off, g.data_size, 0))
        for g in items:
            f.write(
                struct.pack(
                    "<HHhhH",
                    g.width,
                    g.height,
                    g.x_offset,
                    g.y_offset,
                    g.advance,
                )
            )
            f.write(g.bitmap)

    size = out_path.stat().st_size
    print(
        f"Wrote {out_path}  items={n}  file={size} bytes "
        f"({size / (1024 * 1024):.2f} MiB)  "
        f"index@{index_offset} data@{data_offset}"
    )


def verify_fontpack(path: Path, samples: list[tuple[int, int, int]]) -> None:
    """对若干 Key 做文件二分查找，确认与打包内容一致。"""
    data = path.read_bytes()
    magic, ver, _flags, n, index_off, data_off, _res = struct.unpack_from(
        "<4sHHIQQI", data, 0
    )
    if magic != FONTPACK_MAGIC or ver != FONTPACK_VERSION:
        raise SystemExit(f"verify: bad header {magic!r} v{ver}")
    if index_off != HEADER_SIZE or data_off != HEADER_SIZE + n * INDEX_ENTRY_SIZE:
        raise SystemExit("verify: unexpected offsets")

    def read_key(i: int) -> int:
        (k,) = struct.unpack_from("<Q", data, index_off + i * INDEX_ENTRY_SIZE)
        return k

    def bsearch(target: int) -> int | None:
        lo, hi = 0, n - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            k = read_key(mid)
            if k < target:
                lo = mid + 1
            elif k > target:
                hi = mid - 1
            else:
                return mid
        return None

    ok = 0
    for cp, size, bpp in samples:
        key = make_key(cp, size, bpp)
        idx = bsearch(key)
        if idx is None:
            print(f"  verify MISS U+{cp:04X} size={size} bpp={bpp}")
            continue
        _k, doff, dsz, _r = struct.unpack_from(
            "<QIHH", data, index_off + idx * INDEX_ENTRY_SIZE
        )
        w, h, ox, oy, adv = struct.unpack_from("<HHhhH", data, doff)
        bmp_len = dsz - GLYPH_META_SIZE
        ch = chr(cp) if 0x20 <= cp < 0x10000 else "?"
        print(
            f"  verify OK U+{cp:04X} '{ch}' size={size} bpp={bpp} "
            f"box={w}x{h} off=({ox},{oy}) adv={adv} bmp={bmp_len}B"
        )
        ok += 1
    print(f"verify: {ok}/{len(samples)} hits")


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Build fonts.fontpack from TTF/OTF for ESP32 SD binary search"
    )
    ap.add_argument(
        "--ttf",
        type=Path,
        default=None,
        help="单一 TTF/OTF（与 --variants/--sizes 联用；有 --variant-map 时可省略）",
    )
    ap.add_argument(
        "--sizes",
        type=str,
        default="16,20,24,32",
        help="字号列表，如 16,20,24,32 或 16-32",
    )
    ap.add_argument(
        "--bpps",
        type=str,
        default="1,2",
        help="BPP 列表：1=单色，2/4=抗锯齿（与 --sizes 笛卡尔积；有 --variants/--variant-map 时忽略）",
    )
    ap.add_argument(
        "--variants",
        type=str,
        default=None,
        help="精确规格，如 25:1,30:1,30:2（优先于 --sizes/--bpps；需 --ttf）",
    )
    ap.add_argument(
        "--variant-map",
        type=str,
        default=None,
        help="多字重混包：size:bpp=/path/to.ttf,... 例 25:2=/a.ttf,30:4=/b.ttf",
    )
    ap.add_argument(
        "--intervals",
        type=str,
        default="reading_zh",
        help="字集预设/区间（与 fontconvert 相同）；有 --from-ef/--chars-file 时忽略",
    )
    ap.add_argument(
        "--from-ef",
        type=Path,
        default=None,
        help="从已有 .ef 提取码点表（推荐，与现网 ~6000 字一致）",
    )
    ap.add_argument(
        "--chars-file",
        type=Path,
        default=None,
        help="UTF-8 汉字列表文件（逐字）",
    )
    ap.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("fonts.fontpack"),
        help="输出 fonts.fontpack 路径",
    )
    ap.add_argument(
        "--verify",
        action="store_true",
        help="写完后对常见字做二分查找自检",
    )
    args = ap.parse_args()

    variant_map = parse_variant_map(args.variant_map) if args.variant_map else None
    variants = parse_variants(args.variants) if args.variants else None
    if variant_map and variants:
        raise SystemExit("不要同时指定 --variants 与 --variant-map")

    ttf: Path | None = args.ttf
    if variant_map is None:
        if ttf is None:
            ttf = Path("/mnt/f/MiSans/ttf/MiSans-Regular.ttf")
        if not ttf.is_file():
            raise SystemExit(f"TTF not found: {ttf}")

    sizes = parse_int_list(args.sizes)
    bpps = parse_int_list(args.bpps)
    for b in bpps:
        if b not in (1, 2, 4):
            raise SystemExit(f"bpp must be 1, 2 or 4; got {b}")

    if args.chars_file:
        codepoints = load_codepoints_from_chars_file(args.chars_file)
        print(f"charset from {args.chars_file}: {len(codepoints)} codepoints")
    elif args.from_ef:
        codepoints = load_codepoints_from_ef(args.from_ef)
        print(f"charset from {args.from_ef}: {len(codepoints)} codepoints")
    else:
        intervals = resolve_intervals(args.intervals)
        codepoints = []
        for a, b in intervals:
            codepoints.extend(range(a, b + 1))
        codepoints = sorted(set(codepoints))
        print(
            f"charset from intervals={args.intervals!r}: "
            f"{len(codepoints)} codepoints ({len(intervals)} ranges)"
        )
        print(
            "  tip: 全量 CJK 很慢且体积大；生产建议 --from-ef ../epdfont/misans/misans_25_1.ef"
        )

    if variant_map:
        print(
            "Building fontpack (mixed TTF): "
            + ", ".join(f"{s}:{b}←{p.name}" for s, b, p in variant_map)
            + f" chars={len(codepoints)} → expected keys≈"
            f"{len(codepoints) * len(variant_map)}"
        )
        job_variants = [(s, b) for s, b, _ in variant_map]
    elif variants:
        assert ttf is not None
        print(
            f"Building fontpack: ttf={ttf.name} variants={variants} "
            f"chars={len(codepoints)} → expected keys≈"
            f"{len(codepoints) * len(variants)}"
        )
        job_variants = variants
    else:
        assert ttf is not None
        print(
            f"Building fontpack: ttf={ttf.name} sizes={sizes} bpps={bpps} "
            f"chars={len(codepoints)} → expected keys≈"
            f"{len(codepoints) * len(sizes) * len(bpps)}"
        )
        job_variants = [(s, b) for s in sizes for b in bpps]

    t0 = time.time()
    items = build_items(
        ttf, codepoints, sizes, bpps, variants=variants, variant_map=variant_map
    )
    print(f"Rendered {len(items)} glyphs in {time.time() - t0:.1f}s")

    write_fontpack(items, args.output)

    if args.verify and items:
        samples: list[tuple[int, int, int]] = []
        for size, bpp in job_variants:
            samples.extend([(0x4E2D, size, bpp), (0x0041, size, bpp)])
        present = {g.key for g in items}
        samples = [s for s in samples if make_key(*s) in present]
        if not samples:
            g = items[len(items) // 2]
            samples = [(g.codepoint, g.size, g.bpp)]
        verify_fontpack(args.output, samples)


if __name__ == "__main__":
    main()
