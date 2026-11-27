# A2I1 image conversion

[中文](README_zn.md) | **English**

Converts color/grayscale images to 1 bpp `.a2i1`, with preview and binary output. The GUI references the 397 panel; choose appropriate dimensions for other devices. Conversion does not establish physical display quality.

## Environment and GUI

Run from the repository root:

```sh
python -m pip install -r tools/a2i1/requirements.txt
python tools/a2i1/a2i1_gui.py
```

The GUI offers source images, threshold/Floyd/no-dither conversion and A2I1 preview with dimensions, stride and byte size. An available packaged Windows program can run directly; rebuild with `tools/a2i1/build_windows.bat`. A local executable is not required for source execution.

## Command line

```sh
python tools/a2i1/jpg_to_a2i1.py '<source.jpg>'   -o '<output.a2i1>' --size 480x800 --method floyd
```

`threshold` binarizes at a threshold, `floyd` uses Floyd–Steinberg, and `none` quantizes without dithering. `--threshold` sets the threshold and `--preview` writes a PNG preview. Dimensions are examples, not 470 defaults. Choose an output path separate from the original image.

## A2I1 format

```text
0..3    magic A2I1
4..5    width:u16
6..7    height:u16
8..9    stride:u16
10..11  reserved:u16 = 0
12..19  ARGB palette: index 0 black, index 1 white
20..    stride * height bitmap bytes; MSB first, 1 white, 0 black
```

Multibyte integers are little-endian; stride normally rounds width/8 up. The format stores a binary result without requiring a particular dithering method.

## Device use

Device parsing is maintained by [A2UI image implementation](../../main/ui/common/drawing/a2ui/README.md). Select wallpaper/shutdown images through the UI; see [SD storage](../../docs/sd-storage.md). Selected board asset configuration controls built-in packaging; a converter output directory does not define packaging for every board. Keep originals and licenses rather than retaining converted data alone.
