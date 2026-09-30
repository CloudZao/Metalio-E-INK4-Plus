#!/usr/bin/env python3
"""在现有 .ef 字集基础上补空白码点并批量重生成 .ef。

补入：Tab(U+0009)、半角空格(U+0020)、不换行空格(U+00A0)、全角空格(U+3000)。
"""

from __future__ import annotations

import argparse
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

from gen_range import load_codepoints_from_ef, parse_sizes, render_one

WHITESPACE_CPS = (0x0009, 0x0020, 0x00A0, 0x3000)


def extend_codepoints(cps: list[int]) -> list[int]:
    merged = set(cps)
    merged.update(WHITESPACE_CPS)
    return sorted(merged)


def main() -> None:
    ap = argparse.ArgumentParser(description="Extend .ef charset with whitespace and regenerate")
    ap.add_argument(
        "--ttf",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "ttf-fonts/MiSans/MiSans-Light.ttf",
    )
    ap.add_argument(
        "--from-ef",
        type=Path,
        default=Path(__file__).resolve().parent / "MiSans-Light/misans_25_2.ef",
        help="charset template .ef",
    )
    ap.add_argument("--sizes", type=str, default="20-50")
    ap.add_argument("--bpp", type=int, choices=[1, 2], default=2)
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument(
        "--out-dir",
        type=Path,
        default=Path(__file__).resolve().parent / "MiSans-Light",
    )
    ap.add_argument("--prefix", type=str, default="misans")
    args = ap.parse_args()

    if not args.ttf.is_file():
        raise SystemExit(f"TTF not found: {args.ttf}")
    if not args.from_ef.is_file():
        raise SystemExit(f"charset .ef not found: {args.from_ef}")

    base_cps = load_codepoints_from_ef(args.from_ef)
    cps = extend_codepoints(base_cps)
    added = sorted(set(cps) - set(base_cps))
    sizes = parse_sizes(args.sizes)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    print(f"charset: {len(base_cps)} -> {len(cps)}  added={[hex(c) for c in added]}")

    jobs = []
    for sz in sizes:
        out = args.out_dir / f"{args.prefix}_{sz}_{args.bpp}.ef"
        jobs.append((str(args.ttf), sz, args.bpp, cps, str(out)))

    print(f"Generating {len(jobs)} fonts  sizes={sizes[0]}..{sizes[-1]}  bpp={args.bpp}  jobs={args.jobs}")
    print(f"TTF: {args.ttf}")
    print(f"OUT: {args.out_dir}")

    t0 = time.time()
    ok = 0
    failed: list[tuple[int, str]] = []

    with ProcessPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        futs = {ex.submit(render_one, *j): j[1] for j in jobs}
        for fut in as_completed(futs):
            sz = futs[fut]
            try:
                size, path, nbytes, dt = fut.result()
                ok += 1
                print(
                    f"[{ok}/{len(jobs)}] size={size:3d}  {nbytes/1024:.1f} KiB  {dt:.1f}s  {path}",
                    flush=True,
                )
            except Exception as e:  # noqa: BLE001
                failed.append((sz, str(e)))
                print(f"[FAIL] size={sz}: {e}", flush=True)

    print(f"Done in {time.time()-t0:.0f}s  ok={ok}  fail={len(failed)}")
    if failed:
        for sz, err in failed:
            print(f"  fail {sz}: {err}")
        raise SystemExit(1)
    print("Next: cd ../fontpack && ./build_misans_mixed.sh  # 重打含 emoji 的 fontpack")


if __name__ == "__main__":
    main()
