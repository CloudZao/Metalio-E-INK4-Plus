# .ef reading font conversion

[中文](README_zn.md) | **English**

Converts TTF/OTF or LVGL `fmt_txt` sources into single-size `.ef` files for SD-based reading. [fontpack](../fontpack/README.md) owns multi-size EFNT packages; see [font settings](../../docs/ui-font-extension.md) for system fonts and fallback. The device format is implemented in [epdfont.h](../../main/ui/common/drawing/font/epdfont.h) and [epdfont.cc](../../main/ui/common/drawing/font/epdfont.cc).

## Environment

Requires Python 3, FreeType, `freetype-py` and `fonttools`. Bash examples start at the repository root; on Windows use the virtual environment's corresponding `Scripts/python.exe`:

```sh
python3 -m venv tools/epdfont/.venv
tools/epdfont/.venv/bin/pip install -r tools/epdfont/requirements.txt
```

Install the system FreeType runtime if missing on Linux. Retain original font licenses and verify conversion/distribution rights.

## Single-file conversion

```sh
tools/epdfont/.venv/bin/python tools/epdfont/fontconvert.py   --from-ttf '<source-font.ttf>' --size 25 --bpp 2   --intervals reading_zh -o '<output-dir>/misans_25_2.ef'
```

Alternatively use `--from-lvgl-c <font-source.c>` to extract existing LVGL glyphs and codepoints. Outputs follow `{prefix}_{size}_{bpp}.ef`; 1 and 2 bpp are supported. Choose for device rendering needs without assuming a universal size ratio.

`--intervals` accepts `ascii`, `latin1`, `cjk_common`, `cjk_punct`/`punctuation`, `reading_zh` or hexadecimal ranges. Generate only the required set; scanning all CJK increases size and processing time. See `--help` for full arguments.

## Batch sizes

Generate an `.ef` codepoint template first:

```sh
tools/epdfont/.venv/bin/python tools/epdfont/gen_range.py   --ttf '<source-font.ttf>' --from-ef '<template.ef>'   --sizes 10-40 --bpp 2 --jobs 6 --out-dir '<output-dir>'
```

`--sizes` accepts a single value, list or range; `--prefix` sets filenames. Supply font/template paths explicitly instead of relying on local example defaults. `extend_whitespace.py` adds whitespace codepoints; rebuild dependent packages after changing the set.

## Installation and troubleshooting

Copy files into SD `metalio/e-ink/fonts/` and select through reading font settings. Firmware owns lookup rules; do not replace user selection by editing source macros. Keep local environments and outputs separate and verify file integrity before installation.

- Import failure: check the selected Python, requirements and FreeType runtime.
- Missing template: create an `.ef`, then pass `--from-ef`.
- Missing glyph: check the source font and template codepoints; changing size cannot supply absent glyphs.
- Device loading failure: check product directory, format and SD state; back up original fonts before troubleshooting.
