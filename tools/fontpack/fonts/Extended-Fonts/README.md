# Simplified and Traditional Chinese UI and reader extension

[简体中文](README_zn.md)

## Installation

This directory contains the separate SD fontpack, documentation, checksums and original licensing notices. Copy `metalio/e-ink/fonts/extended-fonts.fontpack` to the SD card at:

```text
metalio/e-ink/fonts/extended-fonts.fontpack
```

Upload `extended-fonts.fontpack` directly through the device Transfer page. With automatic classification enabled, `.fontpack` files are saved to `fonts/`, alongside `.ef` fonts. Keep the filename unchanged and choose Replace if it already exists. Reopen the book after saving completes. With classification disabled, open `fonts/` before uploading.

Firmware supporting UI fallback and the specified MiSans `.ef` reader fallback is required. Safely eject and remount the card after copying. File changes do not trigger a redraw. Keep the Flash main fontpack: existing glyphs retain priority. This is a fallback fontpack for UI and the supported reader fonts, not a selectable reader TTF, OTF or `.ef` font, and not a Flash update image.

## Coverage and specifications

Each specification contains 104,774 Unicode characters, including all 102,998 Unicode 17.0 unified and compatibility Han characters: Simplified and Traditional Chinese, the basic block, extensions A through J, and compatibility blocks. Common punctuation, Latin letters and symbols from MiSans are also included. The pack contains 838,192 glyph entries and occupies 255.42 MiB.

Specifications are `20:2`, `25:2`, `30:2`, `35:2`, `40:2`, `45:2`, `50:2` and `30:4` (pixel size:bpp). Reader integration recognizes the fixed filename `misans_<size>_2.ef` at these seven sizes. Existing `.ef` glyphs retain priority; missing glyphs use the same-size extension while preserving the original line height and baseline. No other sizes or runtime scaling are provided. Dedicated formula font specifications are not replaced.

For each codepoint, MiSans takes priority, followed by MiSans L3, then Plangothic. MiSans glyphs at `30:4` use Semibold. L3 and Plangothic glyphs retain their original weight; uncommon characters do not all have a separate bold design.

## Display limitations

Codepoint coverage does not imply regional glyph or variation-sequence coverage. This pack does not automatically choose Mainland, Taiwan or Hong Kong glyph forms for a shared codepoint, handle IVS, ligatures or multi-character shaping, or convert Simplified and Traditional Chinese. Regional differences in source fonts remain. Existing fixed-height controls may clip some tall glyphs. Many SD fallback reads can increase rendering latency.

## Sources and licensing

- [MiSans and MiSans L3, Xiaomi](https://hyperos.mi.com/font/en/download/): MiSans Light is used for all seven 2 bpp sizes; MiSans Semibold is used for 30:4. MiSans L3 supplies additional characters at its original weight.
- [Plangothic V2.9.5795](https://github.com/Losketch/Plangothic-Project/releases/tag/V2.9.5795): P1 and P2 supply remaining Han characters at their original Regular weight. The font is based on Source Han Sans and uses Mainland Chinese glyph conventions.
- [Unicode 17.0 character data](https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt) defines the Han coverage scope.

This fontpack contains bitmap glyphs rendered from these fonts. Source outlines are not included. Preserve the original MiSans agreement and Plangothic SIL Open Font License in `licenses/` when distributing the fontpack as part of the application/device. Each source retains its original licensing terms.

`manifest.json` records glyph counts, source font hashes and the file SHA-256. `SHA256SUMS.txt` also provides the checksum.
