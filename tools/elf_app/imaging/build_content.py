"""Encode existing artwork as format samples and generate the resource index."""
from io import BytesIO
from pathlib import Path
import struct

from PIL import Image, ImageDraw, ImageOps

root = Path(__file__).resolve().parent
repo = root.parents[2]

def load(relative):
    with Image.open(repo / relative) as source:
        image = ImageOps.exif_transpose(source).convert("RGB")
    image.thumbnail((640, 640), Image.Resampling.LANCZOS)
    return image

def encode(image, fmt):
    output = BytesIO()
    image.save(output, format=fmt)
    return output.getvalue()

# Reuse repository artwork without downloading or duplicating source files.
jpeg = encode(load("docs/images/product.png"), "JPEG")
png = encode(load("docs/images/product_en.png"), "PNG")
image = load("tools/a2i1/bg_shutdown2.jpg").convert("L")
width, height = image.size
# A2I1 is inherently binary; dithering preserves the illustration's fine detail.
binary = image.convert("1")
pixels = binary.load()
stride = (width + 7) // 8
bitmap = bytearray(stride * height)
for y in range(height):
    for x in range(width):
        if pixels[x, y] > 127:
            bitmap[y * stride + x // 8] |= 0x80 >> (x % 8)
a2i1 = (b"A2I1" + struct.pack("<HHHH", width, height, stride, 0)
        + bytes((255, 0, 0, 0, 255, 255, 255, 255)) + bitmap)
a2l8 = b"A2L8" + struct.pack("<HH", width, height) + image.tobytes()
samples = [jpeg, png, a2i1, a2l8]
offset = 0
rows = []
for sample in samples:
    rows.append("    {%du, %du}," % (offset, len(sample)))
    offset += len(sample)
(root / "assets.bin").write_bytes(b"".join(samples))
(root / "image_assets.h").write_text(
    "#pragma once\nstatic const struct { unsigned offset, length; } image_assets[] = {\n"
    + "\n".join(rows) + "\n};\n", encoding="utf-8")

icon = Image.new("1", (128, 128), 1)
ink = ImageDraw.Draw(icon)
ink.rectangle((12, 32, 115, 103), outline=0, width=5)
ink.rectangle((30, 20, 60, 32), outline=0, width=5)
ink.ellipse((43, 43, 91, 91), outline=0, width=5)
ink.ellipse((53, 53, 81, 81), fill=0)
icon.save(root / "icon.pbm")
