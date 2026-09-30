#!/usr/bin/env python3
"""Convert a photo/illustration to A2I1 for e-ink / LVGL I1.

Same binary format as A2UI network images (main/display/a2ui).
See a2i1_core.py for layout details.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from a2i1_core import build_a2i1, convert_image, parse_size


def main() -> int:
    ap = argparse.ArgumentParser(description="Convert image → A2I1 (e-ink 1bpp)")
    ap.add_argument("input", type=Path, help="source jpg/png")
    ap.add_argument("-o", "--output", type=Path, help="output .a2i1 (default: <stem>.a2i1)")
    ap.add_argument("--preview", type=Path, help="optional 1-bit PNG preview path")
    ap.add_argument(
        "--method",
        choices=("threshold", "floyd", "none"),
        default="threshold",
        help="binarization (default: threshold = pure B/W)",
    )
    ap.add_argument(
        "--threshold",
        type=int,
        default=128,
        help="gray ≥ N → white when method=threshold (default 128)",
    )
    ap.add_argument("--size", metavar="WxH", help="optional resize, e.g. 480x800")
    args = ap.parse_args()

    if not args.input.is_file():
        print(f"error: not found: {args.input}", file=sys.stderr)
        return 1

    from PIL import Image

    out = args.output or args.input.with_suffix(".a2i1")
    im = Image.open(args.input)
    size = parse_size(args.size) if args.size else None
    bw = convert_image(im, method=args.method, threshold=args.threshold, size=size)
    data = build_a2i1(bw)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    print(f"wrote {out} ({len(data)} bytes, {bw.size[0]}x{bw.size[1]}, method={args.method})")

    if args.preview:
        args.preview.parent.mkdir(parents=True, exist_ok=True)
        bw.save(args.preview)
        print(f"preview {args.preview}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
