#!/usr/bin/env python3
"""A2I1 encode/decode and e-ink-like preview helpers.

Binary layout matches main/display/a2ui (A2UI_I1_MAGIC):

  Offset  Size  Field
  0       4     magic = b'A2I1'
  4       2     width  (px)
  6       2     height (px)
  8       2     stride (bytes per row; typically ceil(width/8))
  10      2     reserved = 0
  12      8     LVGL I1 palette: index0=black, index1=white (ARGB8888 each)
  20      …     bitmap: MSB-left, 1=white, 0=black
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Literal

from PIL import Image, ImageDraw, ImageFilter, ImageEnhance, ImageOps

A2I1_MAGIC = b"A2I1"
HEADER_SIZE = 20

# LVGL I1 palette: ARGB8888 as byte sequence A,R,G,B
PALETTE_BLACK = bytes([0xFF, 0x00, 0x00, 0x00])
PALETTE_WHITE = bytes([0xFF, 0xFF, 0xFF, 0xFF])

# GDEM0397T81P 3.97" — datasheet / Good Display:
#   Resolution 800×480 (module landscape); device LVGL portrait = 480×800
#   Outline 96.62×56.24×0.92 mm; Active 86.40×51.84 mm; pitch 0.108 mm
PANEL_NAME = "GDEM0397T81P"
PANEL_DIAGONAL_INCH = 3.97
PANEL_LOGICAL_W = 480
PANEL_LOGICAL_H = 800
PANEL_PIXEL_PITCH_MM = 0.108
# Landscape module (as in datasheet table)
PANEL_OUTLINE_LANDSCAPE_MM = (96.62, 56.24)
PANEL_ACTIVE_LANDSCAPE_MM = (86.40, 51.84)
# Portrait = device / LVGL orientation
PANEL_OUTLINE_PORTRAIT_MM = (56.24, 96.62)  # W×H mm
PANEL_ACTIVE_PORTRAIT_MM = (51.84, 86.40)

# Symmetric bezel from (outline − active) / 2, converted via pitch → px
_BEZEL_X_MM = (PANEL_OUTLINE_PORTRAIT_MM[0] - PANEL_ACTIVE_PORTRAIT_MM[0]) / 2.0  # 2.20
_BEZEL_Y_MM = (PANEL_OUTLINE_PORTRAIT_MM[1] - PANEL_ACTIVE_PORTRAIT_MM[1]) / 2.0  # 5.11
PANEL_BEZEL_LEFT_PX = max(1, round(_BEZEL_X_MM / PANEL_PIXEL_PITCH_MM))  # ≈20
PANEL_BEZEL_RIGHT_PX = PANEL_BEZEL_LEFT_PX
PANEL_BEZEL_TOP_PX = max(1, round(_BEZEL_Y_MM / PANEL_PIXEL_PITCH_MM))  # ≈47
PANEL_BEZEL_BOTTOM_PX = PANEL_BEZEL_TOP_PX
PANEL_OUTLINE_W_PX = PANEL_LOGICAL_W + PANEL_BEZEL_LEFT_PX + PANEL_BEZEL_RIGHT_PX  # ≈520
PANEL_OUTLINE_H_PX = PANEL_LOGICAL_H + PANEL_BEZEL_TOP_PX + PANEL_BEZEL_BOTTOM_PX  # ≈894

Method = Literal["threshold", "floyd", "none"]

# Approximate electrophoretic reflectance (not LCD pure white/black).
EINK_WHITE = (232, 232, 224)  # warm paper white
EINK_BLACK = (28, 28, 26)  # soft black
EINK_BEZEL = (36, 36, 34)  # glass / black mask around AA
EINK_FRAME = (22, 22, 20)  # module outline rim
EINK_FRAME_EDGE = (10, 10, 9)
EINK_FPC = (55, 48, 28)  # FPC stub hint (bottom, short side when portrait)


@dataclass(frozen=True)
class A2i1Info:
    width: int
    height: int
    stride: int
    reserved: int
    file_size: int
    payload_size: int

    @property
    def expected_payload(self) -> int:
        return self.stride * self.height

    @property
    def panel_hint(self) -> str:
        if self.width == PANEL_LOGICAL_W and self.height == PANEL_LOGICAL_H:
            return f"{PANEL_NAME} 竖屏逻辑 {PANEL_LOGICAL_W}×{PANEL_LOGICAL_H}"
        if self.width == PANEL_LOGICAL_H and self.height == PANEL_LOGICAL_W:
            return f"{PANEL_NAME} 横屏物理 {PANEL_LOGICAL_H}×{PANEL_LOGICAL_W}"
        return f"非面板原生尺寸（面板 {PANEL_LOGICAL_W}×{PANEL_LOGICAL_H}）"


class A2i1Error(ValueError):
    pass


def pack_i1_bitmap(bw: Image.Image) -> tuple[bytes, int]:
    """Pack mode '1' image (1=white) into LVGL I1 row-major MSB-left bytes."""
    if bw.mode != "1":
        raise ValueError(f"expected mode '1', got {bw.mode}")
    w, h = bw.size
    stride = (w + 7) // 8
    px = bw.load()
    out = bytearray(stride * h)
    for y in range(h):
        row = y * stride
        for x in range(w):
            if px[x, y]:
                out[row + (x >> 3)] |= 0x80 >> (x & 7)
    return bytes(out), stride


def unpack_i1_bitmap(data: bytes, width: int, height: int, stride: int) -> Image.Image:
    """Decode LVGL I1 bitmap (1=white) to Pillow mode '1'."""
    need = stride * height
    if len(data) < need:
        raise A2i1Error(f"bitmap truncated: need {need}, got {len(data)}")
    im = Image.new("1", (width, height), 0)
    px = im.load()
    for y in range(height):
        row = y * stride
        for x in range(width):
            byte = data[row + (x >> 3)]
            if byte & (0x80 >> (x & 7)):
                px[x, y] = 1
    return im


def to_bw(im: Image.Image, method: Method, threshold: int = 128) -> Image.Image:
    gray = im.convert("L")
    if method == "threshold":
        return gray.point(lambda p: 255 if p >= threshold else 0, mode="1")
    if method == "floyd":
        return gray.convert("1", dither=Image.Dither.FLOYDSTEINBERG)
    if method == "none":
        return gray.convert("1", dither=Image.Dither.NONE)
    raise ValueError(f"unknown method: {method}")


def build_a2i1(bw: Image.Image) -> bytes:
    bitmap, stride = pack_i1_bitmap(bw)
    w, h = bw.size
    hdr = struct.pack("<4sHHHH", A2I1_MAGIC, w, h, stride, 0)
    return hdr + PALETTE_BLACK + PALETTE_WHITE + bitmap


def parse_a2i1(data: bytes) -> tuple[A2i1Info, Image.Image]:
    if len(data) < HEADER_SIZE:
        raise A2i1Error(f"file too small ({len(data)} bytes)")
    magic, width, height, stride, reserved = struct.unpack_from("<4sHHHH", data, 0)
    if magic != A2I1_MAGIC:
        raise A2i1Error(f"bad magic {magic!r}, expected {A2I1_MAGIC!r}")
    if width == 0 or height == 0 or stride == 0:
        raise A2i1Error("width/height/stride must be non-zero")
    if stride < (width + 7) // 8:
        raise A2i1Error(f"stride {stride} too small for width {width}")
    payload = data[HEADER_SIZE:]
    info = A2i1Info(
        width=width,
        height=height,
        stride=stride,
        reserved=reserved,
        file_size=len(data),
        payload_size=len(payload),
    )
    if len(payload) < info.expected_payload:
        raise A2i1Error(
            f"payload truncated: need {info.expected_payload}, got {len(payload)}"
        )
    bw = unpack_i1_bitmap(payload, width, height, stride)
    return info, bw


def load_a2i1(path: Path | str) -> tuple[A2i1Info, Image.Image]:
    return parse_a2i1(Path(path).read_bytes())


def save_a2i1(path: Path | str, bw: Image.Image) -> bytes:
    data = build_a2i1(bw)
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)
    return data


def convert_image(
    im: Image.Image,
    *,
    method: Method = "threshold",
    threshold: int = 128,
    size: tuple[int, int] | None = None,
) -> Image.Image:
    if size is not None:
        im = im.resize(size, Image.Resampling.LANCZOS)
    return to_bw(im, method, threshold)


def _bw_to_eink_rgb(bw: Image.Image, *, soft: bool, contrast: float) -> Image.Image:
    if bw.mode != "1":
        bw = bw.convert("1")
    rgb = ImageOps.colorize(bw.convert("L"), black=EINK_BLACK, white=EINK_WHITE)
    if soft:
        # Mild blur approximates microcapsule edge diffusion on e-paper.
        rgb = rgb.filter(ImageFilter.GaussianBlur(radius=0.45))
        rgb = ImageEnhance.Contrast(rgb).enhance(1.15)
    if contrast != 1.0:
        rgb = ImageEnhance.Contrast(rgb).enhance(contrast)
    return rgb


def render_panel_module(
    bw: Image.Image | None = None,
    *,
    scale: float = 1.0,
    soft: bool = True,
    contrast: float = 0.92,
) -> Image.Image:
    """Composite image onto a GDEM0397T81P portrait module replica.

    - Active area fixed 480×800 (device logical).
    - Content pasted at AA origin (0,0); clipped if larger; rest stays e-ink white.
    - Bezel sizes from datasheet outline−active (portrait), via 0.108 mm pitch.
    - Default scale 1.0 = 1 device pixel → 1 preview pixel.
    """
    aa_w, aa_h = PANEL_LOGICAL_W, PANEL_LOGICAL_H
    bl, br = PANEL_BEZEL_LEFT_PX, PANEL_BEZEL_RIGHT_PX
    bt, bb = PANEL_BEZEL_TOP_PX, PANEL_BEZEL_BOTTOM_PX
    out_w = aa_w + bl + br
    out_h = aa_h + bt + bb

    # Module outline + black-mask bezel + active area (cleared white)
    module = Image.new("RGB", (out_w, out_h), EINK_FRAME)
    draw = ImageDraw.Draw(module)
    draw.rectangle((0, 0, out_w - 1, out_h - 1), outline=EINK_FRAME_EDGE)

    bezel = Image.new("RGB", (aa_w + 2, aa_h + 2), EINK_BEZEL)
    module.paste(bezel, (bl - 1, bt - 1))

    active = Image.new("RGB", (aa_w, aa_h), EINK_WHITE)
    if bw is not None:
        content = _bw_to_eink_rgb(bw, soft=soft, contrast=contrast)
        # Device style: draw from AA (0,0); clip to panel
        cw, ch = content.size
        crop = content.crop((0, 0, min(cw, aa_w), min(ch, aa_h)))
        active.paste(crop, (0, 0))
    module.paste(active, (bl, bt))

    # FPC stub on bottom (connector on short side of landscape module)
    fpc_h = max(6, bb // 2)
    fpc_w = max(40, aa_w // 3)
    fpc = Image.new("RGB", (fpc_w, fpc_h), EINK_FPC)
    module.paste(fpc, ((out_w - fpc_w) // 2, out_h - fpc_h - 1))

    if scale != 1.0 and scale > 0:
        nw = max(1, int(round(out_w * scale)))
        nh = max(1, int(round(out_h * scale)))
        # 1:1 / integer zoom keep pixel-true; downscale can bilinear
        resample = Image.Resampling.NEAREST if scale >= 1.0 else Image.Resampling.BILINEAR
        module = module.resize((nw, nh), resample)
    return module


def render_eink_preview(
    bw: Image.Image,
    *,
    scale: float = 1.0,
    soft: bool = True,
    contrast: float = 0.92,
    bezel: bool = True,
) -> Image.Image:
    """Preview helper: full module with datasheet bezel (bezel=False → AA only)."""
    if bezel:
        return render_panel_module(bw, scale=scale, soft=soft, contrast=contrast)
    rgb = _bw_to_eink_rgb(bw, soft=soft, contrast=contrast)
    if scale != 1.0 and scale > 0:
        nw = max(1, int(round(bw.size[0] * scale)))
        nh = max(1, int(round(bw.size[1] * scale)))
        resample = Image.Resampling.NEAREST if scale >= 1.0 else Image.Resampling.BILINEAR
        rgb = rgb.resize((nw, nh), resample)
    return rgb


def parse_size(text: str) -> tuple[int, int]:
    w_s, h_s = text.lower().replace(" ", "").split("x", 1)
    w, h = int(w_s), int(h_s)
    if w <= 0 or h <= 0:
        raise ValueError("size must be positive")
    return w, h
