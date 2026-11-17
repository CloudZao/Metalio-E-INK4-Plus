# UI and reader extension fontpack

[简体中文](ui-font-extension_zn.md)

## Installation

The UI uses the main fontpack in the Flash `font_data` partition first. It queries the SD extension only when a glyph is missing. Copy the extension to:

```text
metalio/e-ink/fonts/extended-fonts.fontpack
```

Upload `extended-fonts.fontpack` directly through the device Transfer page. With automatic classification enabled, `.fontpack` files are saved to `fonts/`, alongside `.ef` fonts. Keep the filename unchanged and choose Replace if it already exists. Reopen the book after saving completes. With classification disabled, open `fonts/` before uploading.

The filename is fixed. `SD_PATH_FONT_EXT` in `sd_paths.h` defines the full device path. The file uses EFNT v1 format. It does not write Flash or replace existing glyphs. TTF, OTF and reader `.ef` files cannot serve directly as UI extensions.

UI fallback applies to LVGL fonts created by `fontpack_lv_font_get()`, including the default UI font. The Flash main fontpack and the requested UI specification must remain available.

Reader fonts named `misans_20_2.ef`, `misans_25_2.ef`, `misans_30_2.ef`, `misans_35_2.ef`, `misans_40_2.ef`, `misans_45_2.ef` and `misans_50_2.ef` also support SD fallback at the same pixel size and 2 bpp, for chapter headings and body text. Existing `.ef` glyphs, line height and baseline retain priority. Reader fallback does not require the same size in Flash. Integration recognizes these filenames; other `.ef` names, directly loaded TTF/OTF fonts, separate icon fonts, pixel fonts and startup drawing that calls the Flash glyph API directly are outside this mechanism.

The Simplified and Traditional Chinese fontpack is maintained as a separate SD resource and is not embedded in firmware. See [Extended Fonts](../tools/fontpack/fonts/Extended-Fonts/README.md) for the fontpack, installation, checksums and original licensing notices.

## Building the file

Run the following from the repository root. The Python environment needs `freetype-py`. The source font must contain the extra characters; `extra-chars.txt` is a UTF-8 character list.

```sh
python tools/fontpack/build_fontpack.py --ttf YourFont.ttf --variants 20:2,25:2,30:2,35:2,40:2,45:2,50:2,30:4 --chars-file extra-chars.txt -o extended-fonts.fontpack --verify
```

`30:2` means 30 pixels at 2 bpp; `30:4` means 30 pixels at 4 bpp. Build the specifications used by the target UI. Lookup matches Unicode, size and bpp exactly, without substituting another specification or scaling on the device. Match the main font's appearance, baseline and height to avoid clipping and inconsistent layout.

The packed bitmap limit per glyph is 1024 bytes, defined by `FONT_GLYPH_BMP_MAX`. Evaluate cache and task stack budgets before increasing it. Include only the required characters to reduce file size and SD lookup work.

## Runtime behavior and limitations

- Flash glyphs retain zero-copy mmap access. SD lookup opens the file, performs a binary search, reads one glyph and closes the file before returning, allowing unmount and USB storage transitions.
- Glyphs and missing-glyph results share a 16-entry cache. About 17 KB of PSRAM is allocated after the first access to a valid extension file; the whole pack is never loaded. Entries expire after 5 seconds and are invalidated when the mount generation changes.
- SD cache pointers are never exposed as persistent LVGL bitmaps. Drawing copies the glyph and expands it into the buffer supplied by LVGL.
- An absent card, suspended SD access, missing file, unmatched specification or damaged pack makes the extension unavailable. Existing missing-glyph behavior applies; built-in glyphs remain available. Read errors and corruption are logged, with at least 5 seconds between retries after failure.
- Stop device filesystem access or safely unmount before replacing the file. Remount after copying is complete. Without remounting, old cache entries may remain for up to 5 seconds. File changes do not trigger a UI redraw.
- Cold reads require SD I/O. Many consecutive missing glyphs add layout and drawing latency; keep frequently used core UI glyphs in Flash.
