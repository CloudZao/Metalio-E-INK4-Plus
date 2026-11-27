# English Reader

[中文](README_zn.md) | **English**

## Features and usage

Bundles offline chapters, 206 vocabulary cards and multiple-choice practice. Reading position and quiz results last for the current run only.

## Example

Open a chapter from the contents and page through it, then switch to vocabulary or practice. Reopening resets the session. The app does not read user library files.

## Source and call flow

`build_content.py` converts `book.txt` and `words.tsv` into `assets.bin` and `content.h`. `Show` formats the current mode; `Click` updates chapters, pages and quiz state. Resources use host `read_asset` on demand.

## Compatibility and limits

Requires at least 600×1000 and the asset-reading tail interface. Lewis Carroll's Alice’s Adventures in Wonderland comes from [Project Gutenberg ebook 11](https://www.gutenberg.org/ebooks/11). Keep the complete license in `book.txt` and the reading appendix; check copyright conditions in the distribution jurisdiction.

[english_reader.c](english_reader.c) · [Shared interfaces and lifecycle](../README.md#5-capability-interfaces)

## Build and installation

From the repository root, activate the matching ESP toolchain and set `$compiler` to the full RISC-V compiler path:

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/english_reader -Target esp32s31 -OutputDirectory ./tools/elf_app/english_reader/build/470
```

Output is `build/470/english_reader.elf` relative to this app. See [shared instructions](../README.md#8-publishing-and-device-operations) for the environment, installation manifest and publication requirements.
