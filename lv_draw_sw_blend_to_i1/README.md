# LVGL I1 字形落墨补丁

针对 `managed_components/lvgl__lvgl/src/draw/sw/blend/lv_draw_sw_blend_to_i1.c`。

重拉 / 升级 `lvgl`（如 9.3 → 9.5）后，该文件会被上游覆盖，**必须重新打补丁**，否则墨水屏文字会回到默认 AA 量化，观感变粗/发糊。

## 做什么

在 `lv_draw_sw_blend_color_to_i1` 的 mask 路径接入 `main/display/epd_i1_glyph_thin.h`：

- `#include "epd_i1_glyph_thin.h"`
- 调用 `epd_i1_glyph_mask_hit(...)`：默认 CrossPoint BW「非白即黑」；阅读「抗锯齿」开时 I1 仍实心，覆盖度写入 L8 sidecar，由 flush 走真 4 灰

策略逻辑在头文件里改；本目录脚本只负责把钩子挂进 LVGL 源文件。

## 用法

在项目根目录执行：

```bash
# 打补丁（幂等；已打过会跳过或只修正）
python3 lv_draw_sw_blend_to_i1/apply_patch.py

# 仅检查
python3 lv_draw_sw_blend_to_i1/apply_patch.py --check
```

成功示例：

```text
patched: .../lv_draw_sw_blend_to_i1.c
# 或
already patched: .../lv_draw_sw_blend_to_i1.c
OK: 已打补丁 — ...
```

## 相关文件

| 路径 | 说明 |
|------|------|
| `lv_draw_sw_blend_to_i1/apply_patch.py` | 打补丁脚本 |
| `main/display/epd_i1_glyph_thin.h` | 落墨策略（不随 managed 丢失） |
| `main/CMakeLists.txt` | 给 `lvgl__lvgl` 增加 `display/` include |
| `managed_components/.../lv_draw_sw_blend_to_i1.c` | 被改动的上游文件（gitignore） |

## 建议时机

1. `idf.py` 重新拉取 `managed_components` 之后  
2. 升级 `lvgl/lvgl` 版本之后  
3. 发现阅读页文字突然变粗、或「抗锯齿」开关无效果时，先 `--check`

构建时 `main/CMakeLists.txt` 会幂等执行本脚本；仍建议在重拉依赖后手动 `--check` 一次。

兼容 LVGL **9.3 / 9.5** 上游结构。若大版本改了函数结构，脚本会报错，需按新源码改 `apply_patch.py`。
