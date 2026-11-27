# Firmware resource generators

[中文](README_zn.md) | **English**

This directory owns built-in firmware resource generation/compression, not app-private assets or delivery packages. Original assets stay in their resource directories; app `build_content.py` scripts stay with apps.

## Automatic build steps

- `generate_home_icon_pixels.py`: lossless tiled home icons, invoked by `main/assets/assets.cmake`.
- `generate_quick_control_icons.py`: quick-control icon data, invoked by the same CMake file.
- `compress_binary.py`: length-prefixed zlib data used for built-in shutdown images. Source and output are positional arguments.
- `compress_lvgl_font.py`: native LVGL `fmt_txt` bitmap compression, used by firmware font builds and imported by the pixel-font generator.
- `gen_lang.py`: bilingual runtime strings and sound embeddings, generated from `main/assets/` language resources.
- `pack_ap_fileman.py`: local file-manager web resources, with HTML, translations and output supplied by the build.
- `attach_upgrade_resources.py`: post-build resource attachment invoked by root CMake. It changes the selected output image, not original pictures.

Consult script `--help` and CMake callers for exact arguments. Image/font generation needs Pillow; other dependencies follow script imports. Outputs overwrite their selected paths; never point them at original assets.

## Manual regeneration

`generate_pixel_theme_font.py` builds pixel-theme numeric subsets from bundled Latin Modern Math and imports the adjacent `compress_lvgl_font.py`. Font sources/licenses remain in `tools/ttf-fonts/LatinModernMath/`; resource lookup is relative to `tools/`. Inspect `--help`, generated content and attribution when changing the generator.

See [UI documentation](../../main/ui/README.md) for callers and memory constraints and [partitions](../../partitions/README.md) for Flash resources. Reading font conversion, font packaging and A2I1 conversion have their own [tool entries](../README.md).
