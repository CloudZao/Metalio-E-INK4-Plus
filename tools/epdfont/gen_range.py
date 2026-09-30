#!/usr/bin/env python3
"""Batch-generate MiSans .ef fonts for pixel sizes 10..200.

Charset: reuse codepoints from an existing .ef (default misans/misans_25_1.ef, ~6006 glyphs),
so output matches the product subset (not full CJK plane — that would be tens of GB).

Usage:
  .venv/bin/python gen_range.py \\
    --ttf /mnt/e/Fonts/MiSans/MiSans/ttf/MiSans-Regular.ttf \\
    --from-ef misans/misans_25_1.ef \\
    --sizes 10-40 --bpp 1 --jobs 6 \\
    --out-dir misans
  # output: misans/misans_{size}_{bpp}.ef  e.g. misans_25_1.ef / misans_25_2.ef
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

# Allow importing sibling fontconvert
sys.path.insert(0, str(Path(__file__).resolve().parent))
from fontconvert import Glyph, pack_glyphs_to_xzf  # noqa: E402


def load_codepoints_from_ef(path: Path) -> list[int]:
    data = path.read_bytes()
    magic, ver, flags, lh, bl, asc, desc, ic, gc, io, go, bo = struct.unpack_from(
        "<8sHHHHhhIIIII", data, 0
    )
    if not magic.startswith(b"EPDFONT") or ver != 1:
        raise SystemExit(f"bad ef: {path}")
    cps: list[int] = []
    for i in range(ic):
        first, last, _gidx = struct.unpack_from("<III", data, io + i * 12)
        cps.extend(range(first, last + 1))
    cps = sorted(set(cps))
    print(f"charset from {path.name}: {len(cps)} codepoints, glyphs_hdr={gc}")
    return cps


def render_one(
    ttf: str,
    size: int,
    bpp: int,
    codepoints: list[int],
    out_path: str,
) -> tuple[int, str, int, float]:
    """Worker: render one size. Returns (size, out_path, nbytes, seconds)."""
    import freetype

    t0 = time.time()
    face = freetype.Face(ttf)
    face.set_pixel_sizes(0, size)
    glyphs: list[Glyph] = []

    for cp in codepoints:
        load_cp = cp
        tab_mul = 1
        idx = face.get_char_index(cp)
        if idx == 0 and cp != 0:
            # MiSans 等字体常无 Tab 字形；与 epdfont 一致：复用空格、宽度 ×2
            if cp == 0x09:
                load_cp = 0x20
                tab_mul = 2
            else:
                continue
        flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO
        if bpp == 2:
            flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL
        face.load_char(load_cp, flags)
        g = face.glyph
        bm = g.bitmap
        w, h = bm.width, bm.rows
        adv = ((g.advance.x + 32) >> 6) * tab_mul
        ox = g.bitmap_left
        oy = g.bitmap_top - h

        if w > 255 or h > 255:
            # epdfont glyph w/h are uint8 — clamp box by skipping oversize ink
            # (should not happen for size<=200 typical CJK)
            scale_note = f"skip oversize U+{cp:04X} {w}x{h}"
            print(scale_note, flush=True)
            continue

        if w == 0 or h == 0:
            glyphs.append(Glyph(cp, 0, 0, max(adv, 1), 0, 0, b"\x00"))
            continue

        buf = bytearray(bm.buffer)
        if bpp == 1:
            pitch = bm.pitch
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
            pitch = bm.pitch
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

        glyphs.append(Glyph(cp, w, h, adv, ox, oy, packed))

    asc = face.size.ascender >> 6
    desc = -(face.size.descender >> 6)
    line_height = max(asc + desc, size)
    base_line = max(desc, 0)
    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    pack_glyphs_to_xzf(glyphs, bpp, line_height, base_line, out)
    dt = time.time() - t0
    return size, out_path, out.stat().st_size, dt


def parse_sizes(spec: str) -> list[int]:
    """'10-200' or '10,12,14' or '10-20,25,30-32'."""
    out: list[int] = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-", 1)
            lo, hi = int(a), int(b)
            if lo > hi:
                lo, hi = hi, lo
            out.extend(range(lo, hi + 1))
        else:
            out.append(int(part))
    return sorted(set(out))


def main() -> None:
    ap = argparse.ArgumentParser(description="Batch generate MiSans .ef sizes")
    ap.add_argument(
        "--ttf",
        type=Path,
        default=Path("/mnt/e/Fonts/MiSans/MiSans/ttf/MiSans-Regular.ttf"),
    )
    ap.add_argument(
        "--from-ef",
        type=Path,
        default=Path(__file__).resolve().parent / "misans" / "misans_25_1.ef",
        help="existing .ef whose codepoints define the charset",
    )
    ap.add_argument("--sizes", type=str, default="10-40")
    ap.add_argument("--bpp", type=int, choices=[1, 2], default=1)
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument(
        "--out-dir",
        type=Path,
        default=Path(__file__).resolve().parent / "misans",
    )
    ap.add_argument(
        "--prefix",
        type=str,
        default="misans",
        help="output name: {prefix}_{size}_{bpp}.ef",
    )
    args = ap.parse_args()

    if not args.ttf.is_file():
        raise SystemExit(f"TTF not found: {args.ttf}")
    if not args.from_ef.is_file():
        raise SystemExit(f"charset .ef not found: {args.from_ef}")

    cps = load_codepoints_from_ef(args.from_ef)
    sizes = parse_sizes(args.sizes)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    jobs = []
    for sz in sizes:
        out = args.out_dir / f"{args.prefix}_{sz}_{args.bpp}.ef"
        jobs.append((str(args.ttf), sz, args.bpp, cps, str(out)))

    print(
        f"Generating {len(jobs)} fonts  sizes={sizes[0]}..{sizes[-1]}  "
        f"bpp={args.bpp}  charset={len(cps)}  jobs={args.jobs}"
    )
    print(f"TTF: {args.ttf}")
    print(f"OUT: {args.out_dir}")

    t0 = time.time()
    ok = 0
    failed: list[tuple[int, str]] = []

    # ProcessPool needs picklable top-level function
    with ProcessPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        futs = {ex.submit(render_one, *j): j[1] for j in jobs}
        for fut in as_completed(futs):
            sz = futs[fut]
            try:
                size, path, nbytes, dt = fut.result()
                ok += 1
                print(
                    f"[{ok}/{len(jobs)}] size={size:3d}  {nbytes/1024:.0f} KiB  {dt:.1f}s  {path}",
                    flush=True,
                )
            except Exception as e:  # noqa: BLE001
                failed.append((sz, str(e)))
                print(f"[FAIL] size={sz}: {e}", flush=True)

    print(
        f"Done in {time.time()-t0:.0f}s  ok={ok}  fail={len(failed)}  "
        f"out={args.out_dir}"
    )
    if failed:
        for sz, err in failed:
            print(f"  fail {sz}: {err}")
        raise SystemExit(1)


if __name__ == "__main__":
    main()
