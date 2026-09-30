#!/usr/bin/env python3
"""
重拉 managed_components/lvgl__lvgl 后，给 I1 blend 打上 EPD 字形落墨补丁。

用法（项目根目录）:
  python3 lv_draw_sw_blend_to_i1/apply_patch.py
  python3 lv_draw_sw_blend_to_i1/apply_patch.py --check

依赖 main/display/epd_i1_glyph_thin.h（CMakeLists 已把 display/ 加到 lvgl include）。
兼容 LVGL 9.3 / 9.5 上游 lv_draw_sw_blend_to_i1.c。
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TARGET = (
    ROOT
    / "managed_components"
    / "lvgl__lvgl"
    / "src"
    / "draw"
    / "sw"
    / "blend"
    / "lv_draw_sw_blend_to_i1.c"
)
HEADER = ROOT / "main" / "display" / "epd_i1_glyph_thin.h"

INCLUDE_MARKER = '#include "epd_i1_glyph_thin.h"'
INCLUDE_BLOCK = (
    "\n"
    "/* EPD：字形落墨对齐 CrossPoint BW（见 main/display/epd_i1_glyph_thin.h） */\n"
    f"{INCLUDE_MARKER}\n"
)

BASE_XY_BLOCK = (
    "    int32_t bit_ofs = dsc->relative_area.x1 % 8;\n"
    "    const int32_t base_x = dsc->relative_area.x1;\n"
    "    const int32_t base_y = dsc->relative_area.y1;\n"
)

MASKED_PATCH = """\
    /* Masked with full opacity — EPD：CrossPoint BW 非白即黑 */
    else if(mask && opa >= LV_OPA_MAX) {
        if(LV_RESULT_INVALID == LV_DRAW_SW_COLOR_BLEND_TO_I1_WITH_MASK(dsc)) {
            for(int32_t y = 0; y < h; y++) {
                for(int32_t x = 0; x < w; x++) {
                    uint8_t mask_val = mask[x];
                    if(mask_val == LV_OPA_TRANSP) continue;
                    if(!epd_i1_glyph_mask_hit(mask_val, base_x + x, base_y + y)) continue;
                    if(src_color) {
                        set_bit(dest_buf, x + bit_ofs);
                    }
                    else {
                        clear_bit(dest_buf, x + bit_ofs);
                    }
                }
                dest_buf = drawbuf_next_row(dest_buf, dest_stride);
                mask += mask_stride;
            }
        }
    }
    /* Masked with opacity — 同上 */
    else {
        if(LV_RESULT_INVALID == LV_DRAW_SW_COLOR_BLEND_TO_I1_MIX_MASK_OPA(dsc)) {
            for(int32_t y = 0; y < h; y++) {
                for(int32_t x = 0; x < w; x++) {
                    uint8_t mask_val = LV_OPA_MIX2(mask[x], opa);
                    if(mask_val == LV_OPA_TRANSP) continue;
                    if(!epd_i1_glyph_mask_hit(mask_val, base_x + x, base_y + y)) continue;
                    if(src_color) {
                        set_bit(dest_buf, x + bit_ofs);
                    }
                    else {
                        clear_bit(dest_buf, x + bit_ofs);
                    }
                }
                dest_buf = drawbuf_next_row(dest_buf, dest_stride);
                mask += mask_stride;
            }
        }
    }
"""

# 匹配上游 9.3/9.5 的两段 mask 分支（含行首空白，避免注释缩进翻倍）
MASKED_RE = re.compile(
    r"[ \t]*/\* Masked with full opacity[^*]*\*/\s*"
    r"else if\(mask && opa >= LV_OPA_MAX\) \{.*?"
    r"[ \t]*/\* Masked with opacity[^*]*\*/\s*"
    r"else \{.*?\n    \}\n",
    re.S,
)

BIT_OFS_RE = re.compile(
    r"    int32_t bit_ofs = dsc->relative_area\.x1 % 8;\n"
    r"(?:    const int32_t base_x = dsc->relative_area\.x1;\n"
    r"    const int32_t base_y = dsc->relative_area\.y1;\n)?"
)


def is_patched(text: str) -> bool:
    return INCLUDE_MARKER in text and "epd_i1_glyph_mask_hit" in text


def apply_patch(text: str) -> str:
    if INCLUDE_MARKER not in text:
        needle = '#include "../../../stdlib/lv_string.h"\n'
        if needle not in text:
            raise RuntimeError("找不到 lv_string.h include，无法插入 epd_i1_glyph_thin.h")
        text = text.replace(needle, needle + INCLUDE_BLOCK, 1)

    if "base_x = dsc->relative_area.x1" not in text:
        if not BIT_OFS_RE.search(text):
            raise RuntimeError("找不到 bit_ofs 声明，无法插入 base_x/base_y")
        text = BIT_OFS_RE.sub(BASE_XY_BLOCK, text, count=1)

    # 已打补丁时也允许再跑一遍（修正缩进等），只要能匹配 mask 分支
    if not MASKED_RE.search(text):
        if is_patched(text):
            return text
        raise RuntimeError(
            "找不到 Masked with full/opacity 分支；LVGL 源码结构可能已变，需手工适配"
        )
    text = MASKED_RE.sub(MASKED_PATCH, text, count=1)

    if not is_patched(text):
        raise RuntimeError("补丁写入后校验失败")
    return text


def main() -> int:
    ap = argparse.ArgumentParser(description="Apply EPD I1 glyph-thin patch to LVGL blend_to_i1.c")
    ap.add_argument("--target", type=Path, default=DEFAULT_TARGET, help="lv_draw_sw_blend_to_i1.c 路径")
    ap.add_argument("--check", action="store_true", help="仅检查，不修改")
    args = ap.parse_args()

    if not HEADER.is_file():
        print(f"ERROR: 缺少 {HEADER}", file=sys.stderr)
        return 1
    if not args.target.is_file():
        print(f"ERROR: 找不到 {args.target}（先 idf.py 拉依赖）", file=sys.stderr)
        return 1

    text = args.target.read_text(encoding="utf-8")
    if args.check:
        if is_patched(text):
            print(f"OK: 已打补丁 — {args.target}")
            return 0
        print(f"MISSING: 未打补丁 — {args.target}")
        return 2

    try:
        new_text = apply_patch(text)
    except RuntimeError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1

    if new_text == text:
        print(f"already patched: {args.target}")
        return 0

    args.target.write_text(new_text, encoding="utf-8")
    print(f"patched: {args.target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
