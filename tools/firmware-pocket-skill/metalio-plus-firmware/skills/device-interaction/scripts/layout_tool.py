#!/usr/bin/env python3
"""Validate Plus logical rectangles, then emit the SAME rectangles for C++.

PGM is a layout wireframe, not a font-rendered or hardware-verified preview.
No third-party dependency. Coordinates are half-open rectangles.
"""
import argparse
import json
import re
from pathlib import Path

WIDTH, HEIGHT = 684, 1216
CPP_KEYWORDS = set("alignas alignof and and_eq asm atomic_cancel atomic_commit atomic_noexcept auto bitand bitor bool break case catch char char8_t char16_t char32_t class compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype default delete do double dynamic_cast else enum explicit export extern false float for friend goto if inline int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private protected public reflexpr register reinterpret_cast requires return short signed sizeof static static_assert static_cast struct switch synchronized template this thread_local throw true try typedef typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq".split())


def validate(doc):
    if doc.get("width") != WIDTH or doc.get("height") != HEIGHT:
        raise ValueError("Plus logical canvas must be 684x1216")
    ids, rects = set(), []
    for r in doc.get("rects", []):
        name = r.get("id", "")
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", name) or name in ids or name in CPP_KEYWORDS or "__" in name:
            raise ValueError("Invalid or duplicate rectangle id: " + name)
        ids.add(name)
        values = [r.get(k) for k in ("x", "y", "w", "h")]
        if any(type(v) is not int for v in values):
            raise ValueError(name + ": integer geometry required")
        x, y, w, h = values
        if x < 0 or y < 0 or w <= 0 or h <= 0 or x + w > WIDTH or y + h > HEIGHT:
            raise ValueError(name + ": outside canvas or empty")
        if r.get("hit", False):
            for other in rects:
                if not other.get("hit", False):
                    continue
                if x < other["x"] + other["w"] and other["x"] < x + w and y < other["y"] + other["h"] and other["y"] < y + h:
                    if r.get("overlap_group") != other.get("overlap_group") or not r.get("overlap_group"):
                        raise ValueError(name + ": overlapping hit region " + other["id"])
        rects.append(r)
    if not rects:
        raise ValueError("At least one rectangle is required")
    return rects


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("layout", type=Path)
    p.add_argument("--header", type=Path)
    p.add_argument("--wireframe", type=Path)
    p.add_argument("--report", type=Path)
    a = p.parse_args()
    doc = json.loads(a.layout.read_text(encoding="utf-8-sig"))
    rects = validate(doc)
    if a.header:
        lines = ["#pragma once", '#include "plus_canvas.h"', "namespace app_layout {"]
        for r in rects:
            lines.append("constexpr plus_ui::Rect %s{%d,%d,%d,%d};" % (r["id"], r["x"], r["y"], r["w"], r["h"]))
        lines.append("}")
        a.header.parent.mkdir(parents=True, exist_ok=True)
        a.header.write_text("\n".join(lines) + "\n", encoding="ascii")
    if a.wireframe:
        pixels = bytearray([255]) * (WIDTH * HEIGHT)
        for r in rects:
            x, y, w, h = (r[k] for k in ("x", "y", "w", "h"))
            for xx in range(x, x + w):
                pixels[y * WIDTH + xx] = pixels[(y + h - 1) * WIDTH + xx] = 0
            for yy in range(y, y + h):
                pixels[yy * WIDTH + x] = pixels[yy * WIDTH + x + w - 1] = 0
        a.wireframe.parent.mkdir(parents=True, exist_ok=True)
        a.wireframe.write_bytes(f"P5\n{WIDTH} {HEIGHT}\n255\n".encode("ascii") + pixels)
    result = {"status": "LAYOUT_GEOMETRY_OK", "rectangles": len(rects), "preview_scope": "wireframe_only", "font_pixels_verified": False, "hardware_verified": False}
    if a.report:
        a.report.parent.mkdir(parents=True, exist_ok=True)
        a.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
