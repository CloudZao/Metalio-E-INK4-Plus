"""将 LVGL fmt_txt 位图无损重编码为原生带行预滤波的 RLE。"""
import argparse
from pathlib import Path
import re


def encode_pixels(pixels, width, bpp):
    filtered = [value ^ (pixels[i - width] if i >= width else 0)
                for i, value in enumerate(pixels)]
    bits = []

    def put(value, count):
        bits.extend((value >> shift) & 1 for shift in range(count - 1, -1, -1))

    i = 0
    previous = None
    repeated = False
    count = 0
    while i < len(filtered):
        value = filtered[i]
        if not repeated:
            put(value, bpp)
            repeated = previous is not None and previous == value
            previous = value
            count = 0
            i += 1
        elif value != previous:
            put(0, 1)
            put(value, bpp)
            previous = value
            repeated = False
            i += 1
        elif count < 10:
            put(1, 1)
            count += 1
            i += 1
        else:
            run = 1
            while run < 63 and i + run < len(filtered) and filtered[i + run] == previous:
                run += 1
            put(1, 1)
            put(run, 6)
            i += run
            if i < len(filtered):
                previous = filtered[i]
                put(previous, bpp)
                i += 1
            repeated = False
    bits.extend([0] * ((-len(bits)) % 8))
    # LVGL get_bits 在恰好读到字节边界时也会读取下一个字节。
    return bytes(sum(bits[i + j] << (7 - j) for j in range(8))
                 for i in range(0, len(bits), 8)) + b"\0"


def decode_pixels(data, count, width, bpp, bitmap_format):
    position = 0

    def take(size):
        nonlocal position
        value = 0
        for _ in range(size):
            value = (value << 1) | ((data[position // 8] >> (7 - position % 8)) & 1)
            position += 1
        return value

    pixels = []
    previous = 0
    state = 0
    repeats = 0
    for _ in range(count):
        if bitmap_format == 0:
            value = take(bpp)
        elif state == 0:
            value = take(bpp)
            if position != bpp and value == previous:
                state = 1
                repeats = 0
            previous = value
        elif state == 1:
            flag = take(1)
            repeats += 1
            value = previous
            if flag and repeats == 11:
                repeats = take(6)
                state = 2
            if not flag or (repeats == 0 and state == 2):
                value = take(bpp)
                previous = value
                state = 0
        else:
            repeats -= 1
            value = previous
            if repeats == 0:
                value = take(bpp)
                previous = value
                state = 0
        pixels.append(value)
    if bitmap_format == 1:
        for i in range(width, count):
            pixels[i] ^= pixels[i - width]
    return pixels


def compress_source(source, bitmap_name="glyph_bitmap", glyph_name="glyph_dsc"):
    bitmap = re.search(r"\b" + bitmap_name + r"\[\]\s*=\s*\{(.*?)\};", source, re.S)
    glyphs = re.search(r"\b" + glyph_name + r"\[\]\s*=\s*\{(.*?)\};", source, re.S)
    if not bitmap or not glyphs:
        raise ValueError("Missing fmt_txt bitmap or glyph descriptors")
    data = bytes(int(n, 16) for n in re.findall(r"0x([0-9a-fA-F]+)", bitmap[1]))
    bpp = int(re.search(r"\.bpp\s*=\s*(\d+)", source)[1])
    if bpp not in (1, 2, 3, 4):
        raise ValueError("Unsupported fmt_txt pixel depth")
    old_format = re.search(r"\.bitmap_format\s*=\s*(\w+)", source)[1]
    formats = {"0": 0, "1": 1, "2": 2, "LV_FONT_FMT_TXT_COMPRESSED": 1,
               "LV_FONT_FMT_TXT_COMPRESSED_NO_PREFILTER": 2}
    bitmap_format = formats[old_format]
    new_bpp = max(2, bpp)
    packed = bytearray()

    def convert(match):
        fields = dict(re.findall(r"\.(\w+)\s*=\s*(-?\d+)", match[0]))
        if int(fields.get("stride", "0")) != 0:
            raise ValueError("Aligned glyph rows require a separate encoder")
        start, width, height = (int(fields[key]) for key in ("bitmap_index", "box_w", "box_h"))
        pixels = decode_pixels(data[start:], width * height, width, bpp, bitmap_format)
        if bpp == 1:
            pixels = [value * 3 for value in pixels]
        offset = len(packed)
        if pixels:
            encoded = encode_pixels(pixels, width, new_bpp)
            if decode_pixels(encoded, len(pixels), width, new_bpp, 1) != pixels:
                raise ValueError("RLE pixel round trip failed")
            packed.extend(encoded)
        return re.sub(r"(\.bitmap_index\s*=\s*)\d+", lambda m: m[1] + str(offset), match[0])

    descriptions = re.sub(r"\{[^{}]+\}", convert, glyphs[1])
    source = source[:glyphs.start(1)] + descriptions + source[glyphs.end(1):]
    encoded_text = "\n" + "\n".join(
        "    " + ", ".join(f"0x{v:02x}" for v in packed[i:i + 20]) + ","
        for i in range(0, len(packed), 20)) + "\n"
    source = source[:bitmap.start(1)] + encoded_text + source[bitmap.end(1):]
    source = re.sub(r"(\.bpp\s*=\s*)\d+", lambda m: m[1] + str(new_bpp), source)
    source = re.sub(r"(\.bitmap_format\s*=\s*)\w+", r"\g<1>LV_FONT_FMT_TXT_COMPRESSED", source)
    return source


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    source = compress_source(args.input.read_text(encoding="utf-8"))
    source = source.replace('"../../lvgl.h"', '"lvgl.h"')
    args.output.write_text(source, encoding="utf-8", newline="\n")
