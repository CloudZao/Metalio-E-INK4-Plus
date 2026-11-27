# ebook Studio

[中文](README_zn.md) | **English**

Conversion and web reading tools for ESP32-compatible `.ebook` files.

## Components

- `epdbook/`: writer, reader and format extractors.
- `convert_ebook.py`: command-line conversion.
- `web/`: uploads, conversion, library and light/dark web reader.
- [SPEC.md](SPEC.md): binary format specification.
- `test-book/` and optional `book/`: sample books.
- `data/library/`: converted web library files.
- [emulator](emulator/README.md): LVGL WASM/SDL preview; rebuild WASM after reader-layout changes.

## Web interface

Run in `tools/ebook/`:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
./run_web.sh
```

Open the local address printed by the launcher, normally `http://127.0.0.1:8765`. It advances to another port if needed. `EBOOK_PORT=9000 ./run_web.sh` selects a port; `./run_web.sh --kill` stops an earlier instance. The interface converts PDF/EPUB/MOBI/TXT, reads existing `.ebook` files with cover/contents/images, and downloads results. Clear library deletes all converted library outputs.

### Windows executable

From this directory, run `build_exe.bat` on Windows or `./build_exe.sh` in WSL. Packaging copies `tools/epdfont/MiSans-Light/*.ef` into `web/static/fonts/ef/`. Output is `dist/ebook-web.exe`, including the collected fonts. Running starts the local service/browser; closing its console stops the service. Library data is stored in `data/` beside the executable.

WSL packaging uses a temporary Windows working directory and copies output back to `dist/`; the script owns exact paths/environment requirements. Optional settings are `EBOOK_PORT`, `EBOOK_HOST` and `EBOOK_NO_BROWSER=1`.

### TXT chapters

Chapter recognition accepts Chinese chapter/volume headings, prologues/epilogues, `Chapter 1`, `Part 2`, `Volume 3`, and supported short numeric headings. At least two detected chapters set `HAS_TOC`. `--chapter-regex` overrides recognition; `--no-split` creates one chapter.

Large chapters, by default above 256 KiB decompressed including images, split into numbered parts without independently setting `HAS_TOC`. `--no-chapter-size-split` disables this. Current firmware streams physical blocks instead of rejecting a chapter solely for exceeding 384 KiB; keeping splitting reduces waits and supports older firmware.

```sh
.venv/bin/python convert_ebook.py '<novel.txt>' -o '<novel.ebook>' --inspect
```

### Scanned PDF

Use the web whole-page-image option or:

```sh
.venv/bin/python convert_ebook.py '<scan.pdf>' -o '<scan.ebook>'   --pdf-pages-as-images --pdf-page-scale 1.5   --pdf-page-max 480x720 --binarize otsu
```

`--pdf-page-scale` scales relative to 72 dpi. `--pdf-page-max` limits the encoded page bounds, default 480×720; `PAGE_IMAGES` identifies full-page images. `--image-max` controls illustrations; `orig` preserves original dimensions/aspect subject to chunk budgets. Images may raise `chunk_max` up to 49152 to match firmware limits.

### Binarization and color

Default Otsu adaptive thresholds suit line art and many illustrations. Alternatives are `bayer` (4×4 ordered), `dither` (Floyd–Steinberg), `sauvola` for uneven scans, or `fixed --threshold 128`. `--contrast`, `--sauvola-window` and `--sauvola-k` tune processing. PDF/EPUB web uploads provide a preview by page/illustration before conversion.

Use the web keep-color option or:

```sh
.venv/bin/python convert_ebook.py '<book.epub>' -o '<out.ebook>' --keep-color
```

Color uses JPEG rather than A2I1 and remains subject to chunk/cover limits. Device rendering converts to L8 grayscale and Bayer dithering like native EPUB images. `--keep-color` and `--binarize` are mutually exclusive.

## CLI and device reading

`convert_ebook.py <input> -o <output.ebook> --inspect` converts and inspects a file. See `--help` for full options.

Firmware streams TXT, `.ebook` physical blocks and EPUB ZIP content. First paint restores a body anchor while contents and current-chapter pagination continue in background. See [reader scheduling](../../docs/ui-scheduling.md) for progress compatibility and lifecycles. Initial scanning or very long chapters can still take time; format/block/ZIP/file-offset limits apply, and corrupt or encrypted books may be unreadable.

Firmware places pagination work in the reader temporary directory; the desktop emulator uses `.rpages-N.tmp` and its index beside the book. Work needs writable storage and free space. Normal exit/failure reclaims work; handles close before standby SD removal. Old `.idx` is read only when needed for old TXT progress conversion; full-book `.idx`/`.cpg` are no longer generated and old caches are not automatically deleted. `.rsi` is rebuildable; `.pos` and bookmarks are user data to retain.
