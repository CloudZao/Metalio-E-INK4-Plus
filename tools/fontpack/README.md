# .fontpack packaging

[中文](README_zn.md) | **English**

Packages multiple font sizes/bpp variants as EFNT for SD lookup or mapped `font_data`. See [epdfont](../epdfont/README.md) for single-size `.ef` conversion and [font extensions](../../docs/ui-font-extension.md) for installation.

## Environment and build

Requires Python, FreeType and the requirements file. Run these Bash commands in this directory:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
./build_misans_mixed.sh
```

The script uses an existing MiSans-Light `.ef` codepoint template, MiSans, Noto Emoji and Latin Modern Math. Check the script's required sources and licenses first. Output is under `fonts/MiSans-Mixed/` and copied to package entry files here and `main/assets/fonts/font.fontpack`; running modifies these outputs. See [default package](fonts/MiSans-Mixed/README.md) for composition/licenses and [extended fonts](fonts/Extended-Fonts/README.md) for that separate package.

## Custom packages

```sh
.venv/bin/python build_fontpack.py   --ttf '<source-font.ttf>' --from-ef '<template.ef>'   --variants 25:1,30:1,30:2 -o '<output.fontpack>' --verify
```

`--variant-map` assigns fonts per size/bpp; `--sizes` and `--bpps` generate combinations. Codepoints can come from `.ef`, `--chars-file` or `--intervals`. Add Emoji/Math through their merge scripts. Do not overwrite the default product package with one lacking required glyphs. Each script documents full arguments in `--help`.

## EFNT format

```text
Header 32B: magic="EFNT" | version:u16 | flags:u16 | total_items:u32
            index_offset:u64 | data_offset:u64 | reserved:u32
Index 16B: key:u64=(unicode<<32)|(size<<16)|bpp
           data_offset:u32 | data_size:u16 | reserved:u16
Data: width:u16 | height:u16 | x_offset:i16 | y_offset:i16 | advance:u16 | bitmap[]
```

Indexes are sorted by key. Bitmaps are MSB-first, tightly packed without row padding. See [font_loader.h](esp32/font_loader.h), [font_loader.c](esp32/font_loader.c) and [example_usage.c](esp32/example_usage.c).

## Installation and limits

SD packages use the product fonts directory; Flash packages use `font_data`. Flashing replaces the original font set: back up, check model, partition and capacity, and follow [partition documentation](../../partitions/README.md). `flash_fontpack.sh` takes device port and package path and is run only when flashing is intended.

For missing glyphs, match unicode/size/bpp to the stored key. Create missing `.ef` templates with epdfont. For oversized files, reduce codepoints/variants rather than exceeding partition capacity or merely raising buffers. Original font licenses still govern distribution.
