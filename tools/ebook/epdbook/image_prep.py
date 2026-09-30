"""图片预处理：正文插图 vs 封面分轨。

架构约定（墨水屏 / ESP32）：
- 正文 IMAGE 块：解压后 ImagePayload ≤ chunk_max（流式缓冲，默认 4KB）
- 独立封面：一次读入详情页，预算 COVER_MAX_PAYLOAD（与 chunk 解耦）
- 产品默认编码：A2I1（1bpp，与 tools/a2i1 / 固件 DecodeA2i1ToL8 同构）
- 失败策略：返回 None / 抛给上层捕获后跳过，不得半写入损坏块
"""

from __future__ import annotations

import io
from dataclasses import dataclass
from typing import Optional

from .format import (
    COVER_MAX_PAYLOAD,
    DEFAULT_COVER_MAX_H,
    DEFAULT_COVER_MAX_W,
    DEFAULT_IMAGE_MAX_H,
    DEFAULT_IMAGE_MAX_W,
    DEFAULT_IMAGE_MODE,
    IMAGE_PAYLOAD_HEADER,
    IMG_A2I1,
    IMG_GRAY,
    IMG_JPEG,
    IMG_PNG,
)

try:
    from PIL import Image, ImageEnhance
except ImportError:  # pragma: no cover
    Image = None  # type: ignore
    ImageEnhance = None  # type: ignore


BINARIZE_METHODS = ("fixed", "otsu", "sauvola", "dither", "bayer")
DEFAULT_BINARIZE_METHOD = "otsu"


@dataclass
class BinarizeOptions:
    """A2I1 二值化参数（仅影响转换侧；写出仍是 1bpp A2I1）。"""

    method: str = DEFAULT_BINARIZE_METHOD  # fixed|otsu|sauvola|dither|bayer
    threshold: int = 128  # fixed
    contrast: float = 1.0  # 1.0=不变；>1 增强对比
    window: int = 25  # sauvola 奇数窗口
    k: float = 0.34  # sauvola 敏感度


def parse_binarize_method(raw: Optional[str]) -> str:
    m = (raw or DEFAULT_BINARIZE_METHOD).strip().lower()
    aliases = {
        "threshold": "fixed",
        "hard": "fixed",
        "floyd": "dither",
        "fs": "dither",
        "adaptive": "sauvola",
        "local": "sauvola",
        "ordered": "bayer",
        "device": "bayer",
        "epub": "bayer",  # 与设备原生 EPUB GrayToL8Dither 同构
    }
    m = aliases.get(m, m)
    if m not in BINARIZE_METHODS:
        raise ValueError(f"未知二值化方法: {raw}（可选: {', '.join(BINARIZE_METHODS)}）")
    return m


def binarize_options_from_args(
    *,
    method: Optional[str] = None,
    threshold: int = 128,
    contrast: float = 1.0,
    window: int = 25,
    k: float = 0.34,
) -> BinarizeOptions:
    return BinarizeOptions(
        method=parse_binarize_method(method),
        threshold=max(0, min(255, int(threshold))),
        contrast=max(0.2, min(4.0, float(contrast))),
        window=max(3, min(101, int(window) | 1)),  # 强制奇数
        k=max(0.01, min(1.0, float(k))),
    )


@dataclass
class PreparedImage:
    fmt: int
    width: int
    height: int
    stride: int
    data: bytes

    @property
    def payload_size(self) -> int:
        return IMAGE_PAYLOAD_HEADER + len(self.data)


def _ensure_pil() -> None:
    if Image is None:
        raise RuntimeError("Pillow 未安装：pip install Pillow")


def a2i1_file_size(width: int, height: int) -> int:
    """完整 A2I1 文件字节数（含 20B 头）。"""
    stride = (width + 7) // 8
    return 20 + stride * height


def fit_size(width: int, height: int, max_width: int, max_height: int) -> tuple[int, int]:
    """大于框则等比例缩小；已在框内则原样。不放大。

    max_width/max_height ≤ 0 表示不按框缩小（保留原始尺寸）。
    """
    if width <= 0 or height <= 0:
        raise ValueError("invalid size")
    if max_width <= 0 or max_height <= 0:
        return width, height
    scale = min(max_width / width, max_height / height, 1.0)
    if scale >= 1.0:
        return width, height
    return max(1, int(width * scale)), max(1, int(height * scale))


def _apply_contrast(im: "Image.Image", contrast: float) -> "Image.Image":
    if ImageEnhance is None or abs(contrast - 1.0) < 0.01:
        return im
    return ImageEnhance.Contrast(im).enhance(contrast)


def _is_color_mode(mode: str) -> bool:
    """彩色编码模式：保留 RGB，设备侧再按原生 EPUB 同款 Bayer 转黑白。"""
    return mode in ("jpeg", "jpg", "png", "color")


def _histogram(pixels: bytes) -> list[int]:
    hist = [0] * 256
    for p in pixels:
        hist[p] += 1
    return hist


def otsu_threshold(pixels: bytes) -> int:
    """Otsu 全局阈值；空图回退 128。"""
    n = len(pixels)
    if n == 0:
        return 128
    hist = _histogram(pixels)
    sum_all = sum(i * hist[i] for i in range(256))
    sum_b = 0
    w_b = 0
    max_var = -1.0
    best = 128
    for t in range(256):
        w_b += hist[t]
        if w_b == 0:
            continue
        w_f = n - w_b
        if w_f == 0:
            break
        sum_b += t * hist[t]
        m_b = sum_b / w_b
        m_f = (sum_all - sum_b) / w_f
        var = w_b * w_f * (m_b - m_f) ** 2
        if var > max_var:
            max_var = var
            best = t
    return best


def _integral_sum(pixels: bytes, w: int, h: int) -> list[int]:
    """(h+1)*(w+1) 积分图，便于 O(1) 矩形求和。"""
    W = w + 1
    integ = [0] * ((h + 1) * W)
    for y in range(h):
        row = 0
        base = y * w
        for x in range(w):
            row += pixels[base + x]
            integ[(y + 1) * W + (x + 1)] = integ[y * W + (x + 1)] + row
    return integ


def _integral_sq(pixels: bytes, w: int, h: int) -> list[int]:
    W = w + 1
    integ = [0] * ((h + 1) * W)
    for y in range(h):
        row = 0
        base = y * w
        for x in range(w):
            v = pixels[base + x]
            row += v * v
            integ[(y + 1) * W + (x + 1)] = integ[y * W + (x + 1)] + row
    return integ


def _rect(integ: list[int], w: int, x0: int, y0: int, x1: int, y1: int) -> int:
    W = w + 1
    return integ[y1 * W + x1] - integ[y0 * W + x1] - integ[y1 * W + x0] + integ[y0 * W + x0]


def _sauvola_binarize(pixels: bytes, w: int, h: int, window: int, k: float) -> bytes:
    """Sauvola 局部阈值 → 0/255。R 固定 128。"""
    half = max(1, window // 2)
    integ = _integral_sum(pixels, w, h)
    integ2 = _integral_sq(pixels, w, h)
    out = bytearray(w * h)
    R = 128.0
    for y in range(h):
        y0 = max(0, y - half)
        y1 = min(h, y + half + 1)
        row = y * w
        for x in range(w):
            x0 = max(0, x - half)
            x1 = min(w, x + half + 1)
            area = (x1 - x0) * (y1 - y0)
            if area < 1:
                out[row + x] = 255 if pixels[row + x] >= 128 else 0
                continue
            s = _rect(integ, w, x0, y0, x1, y1)
            s2 = _rect(integ2, w, x0, y0, x1, y1)
            mean = s / area
            var = max(0.0, s2 / area - mean * mean)
            std = var**0.5
            thresh = mean * (1.0 + k * (std / R - 1.0))
            out[row + x] = 255 if pixels[row + x] >= thresh else 0
    return bytes(out)


def _floyd_steinberg(pixels: bytes, w: int, h: int) -> bytes:
    """误差扩散抖动 → 0/255（与 PIL FLOYDSTEINBERG 语义接近）。"""
    buf = [float(p) for p in pixels]
    out = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            old = buf[i]
            new = 255.0 if old >= 128.0 else 0.0
            out[i] = int(new)
            err = old - new
            if x + 1 < w:
                buf[i + 1] += err * 7 / 16
            if y + 1 < h:
                if x > 0:
                    buf[i + w - 1] += err * 3 / 16
                buf[i + w] += err * 5 / 16
                if x + 1 < w:
                    buf[i + w + 1] += err * 1 / 16
    return bytes(out)


# 与固件 main/reader/image_util.cc GrayToL8Dither / kBayer4 同构
_BAYER4 = (
    (0, 8, 2, 10),
    (12, 4, 14, 6),
    (3, 11, 1, 9),
    (15, 7, 13, 5),
)


def _bayer4_binarize(pixels: bytes, w: int, h: int) -> bytes:
    """4×4 Bayer 有序抖动 → 0/255（对齐设备原生 EPUB 解码）。"""
    out = bytearray(w * h)
    for y in range(h):
        row = y * w
        thr_row = _BAYER4[y & 3]
        for x in range(w):
            gray = pixels[row + x]
            level = (255 - gray) * 16 // 256  # 黑度 0..15
            thr = thr_row[x & 3]
            out[row + x] = 0 if level > thr else 255
    return bytes(out)


def binarize_l(im: "Image.Image", opts: Optional[BinarizeOptions] = None) -> "Image.Image":
    """灰度图 → 仅含 0/255 的 L 图。"""
    _ensure_pil()
    opts = opts or BinarizeOptions()
    if im.mode != "L":
        im = im.convert("L")
    im = _apply_contrast(im, opts.contrast)
    w, h = im.size
    pixels = im.tobytes()
    method = parse_binarize_method(opts.method)

    if method == "fixed":
        thr = max(0, min(255, int(opts.threshold)))
        out = bytes(255 if p >= thr else 0 for p in pixels)
    elif method == "otsu":
        thr = otsu_threshold(pixels)
        out = bytes(255 if p >= thr else 0 for p in pixels)
    elif method == "sauvola":
        out = _sauvola_binarize(pixels, w, h, opts.window, opts.k)
    elif method == "dither":
        out = _floyd_steinberg(pixels, w, h)
    elif method == "bayer":
        out = _bayer4_binarize(pixels, w, h)
    else:
        raise ValueError(f"unknown binarize method: {method}")

    return Image.frombytes("L", (w, h), out)


def prepare_image(
    data: bytes,
    *,
    max_width: int = 480,
    max_height: int = 800,
    mode: str = DEFAULT_IMAGE_MODE,
    jpeg_quality: int = 75,
    threshold: int = 128,
    binarize: Optional[BinarizeOptions] = None,
) -> PreparedImage:
    """单次缩放编码；不保证落在预算内。需要预算请用 prepare_image_fit。"""
    _ensure_pil()
    if not data:
        raise ValueError("empty image data")
    im = Image.open(io.BytesIO(data))
    if _is_color_mode(mode):
        im = im.convert("RGB")
    else:
        im = im.convert("L")
    w, h = im.size
    if w <= 0 or h <= 0:
        raise ValueError("invalid image size")
    nw, nh = fit_size(w, h, max_width, max_height)
    if (nw, nh) != (w, h):
        im = im.resize((nw, nh), Image.Resampling.LANCZOS)
    if _is_color_mode(mode):
        return encode_color_image(im, mode=mode, jpeg_quality=jpeg_quality)
    return encode_gray_image(
        im,
        mode=mode,
        jpeg_quality=jpeg_quality,
        threshold=threshold,
        binarize=binarize,
    )


def encode_color_image(
    im: "Image.Image",
    *,
    mode: str = "jpeg",
    jpeg_quality: int = 75,
) -> PreparedImage:
    """彩色图编码为 JPEG/PNG（不二值化；设备侧 DecodeImageToL8 转黑白）。"""
    _ensure_pil()
    if im.mode != "RGB":
        im = im.convert("RGB")
    w, h = im.size
    if mode == "png":
        buf = io.BytesIO()
        im.save(buf, format="PNG", optimize=True)
        return PreparedImage(IMG_PNG, w, h, 0, buf.getvalue())
    buf = io.BytesIO()
    im.save(buf, format="JPEG", quality=jpeg_quality, optimize=True)
    return PreparedImage(IMG_JPEG, w, h, 0, buf.getvalue())


def encode_gray_image(
    im: "Image.Image",
    *,
    mode: str = DEFAULT_IMAGE_MODE,
    jpeg_quality: int = 75,
    threshold: int = 128,
    binarize: Optional[BinarizeOptions] = None,
) -> PreparedImage:
    _ensure_pil()
    if im.mode != "L":
        im = im.convert("L")
    w, h = im.size
    if mode == "a2i1":
        return to_a2i1(im, threshold=threshold, binarize=binarize)
    if mode == "gray":
        return PreparedImage(IMG_GRAY, w, h, w, im.tobytes())
    if mode == "png":
        buf = io.BytesIO()
        im.save(buf, format="PNG", optimize=True)
        return PreparedImage(IMG_PNG, w, h, 0, buf.getvalue())
    if mode in ("jpeg", "jpg", "none"):
        buf = io.BytesIO()
        im.save(buf, format="JPEG", quality=jpeg_quality, optimize=True)
        return PreparedImage(IMG_JPEG, w, h, 0, buf.getvalue())
    raise ValueError(f"unknown image mode: {mode}")


def to_a2i1(
    im: "Image.Image",
    *,
    threshold: int = 128,
    binarize: Optional[BinarizeOptions] = None,
) -> PreparedImage:
    """与 tools/a2i1 同构的完整 A2I1 文件，供固件 DecodeA2i1ToL8。"""
    _ensure_pil()
    opts = binarize or BinarizeOptions(method="fixed", threshold=threshold)
    bw = binarize_l(im, opts)
    w, h = bw.size
    stride = (w + 7) // 8
    pixels = bw.tobytes()
    bitmap = bytearray(stride * h)
    for y in range(h):
        row = y * w
        for x in range(w):
            if pixels[row + x] >= 128:
                bitmap[y * stride + (x >> 3)] |= 0x80 >> (x & 7)
    header = bytearray()
    header += b"A2I1"
    header += w.to_bytes(2, "little")
    header += h.to_bytes(2, "little")
    header += stride.to_bytes(2, "little")
    header += (0).to_bytes(2, "little")
    header += bytes([0xFF, 0x00, 0x00, 0x00])  # index0 黑
    header += bytes([0xFF, 0xFF, 0xFF, 0xFF])  # index1 白
    return PreparedImage(IMG_A2I1, w, h, stride, bytes(header) + bytes(bitmap))


def prepare_image_fit(
    data: bytes,
    *,
    max_payload: int,
    max_width: int,
    max_height: int,
    mode: str = DEFAULT_IMAGE_MODE,
    jpeg_quality: int = 75,
    threshold: int = 128,
    binarize: Optional[BinarizeOptions] = None,
    min_edge: int = 32,
) -> PreparedImage:
    """
    1) 先按 max_width×max_height 等比例缩小（小于不放大）
    2) 若 ImagePayload 仍 > max_payload，继续等比缩小直至落入预算
    """
    if max_payload <= IMAGE_PAYLOAD_HEADER + 32:
        raise ValueError(f"max_payload too small: {max_payload}")
    _ensure_pil()
    if not data:
        raise ValueError("empty image data")

    color = _is_color_mode(mode)
    base = Image.open(io.BytesIO(data))
    base = base.convert("RGB") if color else base.convert("L")
    bw, bh = base.size
    if bw <= 0 or bh <= 0:
        raise ValueError("invalid image size")

    cur_w, cur_h = fit_size(bw, bh, max_width, max_height)
    color_mode = "jpeg" if mode == "color" else mode

    last_err = ""
    for _ in range(16):
        im = base if (cur_w, cur_h) == (bw, bh) else base.resize(
            (cur_w, cur_h), Image.Resampling.LANCZOS
        )
        if color:
            qualities = [jpeg_quality]
            for q in (85, 75, 65, 55, 45, 35, 25):
                if q not in qualities:
                    qualities.append(q)
            for q in qualities:
                prep = encode_color_image(im, mode=color_mode, jpeg_quality=q)
                if prep.payload_size <= max_payload:
                    return prep
                last_err = f"{prep.payload_size} > {max_payload} at {cur_w}x{cur_h} q={q}"
        else:
            prep = encode_gray_image(
                im,
                mode=mode,
                jpeg_quality=jpeg_quality,
                threshold=threshold,
                binarize=binarize,
            )
            if prep.payload_size <= max_payload:
                return prep
            last_err = f"{prep.payload_size} > {max_payload} at {cur_w}x{cur_h}"
        if cur_w <= min_edge and cur_h <= min_edge:
            break
        cur_w = max(min_edge, int(cur_w * 0.82))
        cur_h = max(min_edge, int(cur_h * 0.82))

    raise ValueError(f"image cannot fit budget: {last_err}")


def prepare_cover(
    data: bytes,
    *,
    mode: str = "a2i1",
    max_width: int = DEFAULT_COVER_MAX_W,
    max_height: int = DEFAULT_COVER_MAX_H,
    max_payload: int = COVER_MAX_PAYLOAD,
    binarize: Optional[BinarizeOptions] = None,
) -> Optional[PreparedImage]:
    """封面：强制 A2I1 语义由调用方保证 mode；失败返回 None。"""
    try:
        return prepare_image_fit(
            data,
            max_payload=max_payload,
            max_width=max_width,
            max_height=max_height,
            mode=mode,
            binarize=binarize,
        )
    except Exception:
        return None


def prepare_inline(
    data: bytes,
    *,
    mode: str = "a2i1",
    max_width: int = DEFAULT_IMAGE_MAX_W,
    max_height: int = DEFAULT_IMAGE_MAX_H,
    chunk_max: int = 4096,
    binarize: Optional[BinarizeOptions] = None,
) -> Optional[PreparedImage]:
    """正文插图：先按 image-max 框缩小，再压进 chunk_max；失败返回 None。"""
    try:
        return prepare_image_fit(
            data,
            max_payload=chunk_max,
            max_width=max_width,
            max_height=max_height,
            mode=mode,
            binarize=binarize,
        )
    except Exception:
        return None


def preview_binarize_pair(
    data: bytes,
    *,
    max_width: int = 360,
    max_height: int = 600,
    binarize: Optional[BinarizeOptions] = None,
) -> tuple["Image.Image", "Image.Image"]:
    """预览用：返回 (灰度缩放图, 二值图)，不编码 A2I1。"""
    _ensure_pil()
    if not data:
        raise ValueError("empty image data")
    im = Image.open(io.BytesIO(data)).convert("L")
    w, h = im.size
    nw, nh = fit_size(w, h, max_width, max_height)
    if (nw, nh) != (w, h):
        im = im.resize((nw, nh), Image.Resampling.LANCZOS)
    opts = binarize or BinarizeOptions()
    gray = _apply_contrast(im, opts.contrast) if abs(opts.contrast - 1.0) >= 0.01 else im
    # 预览灰度侧也显示对比度，便于对照
    binary = binarize_l(im, opts)
    return gray, binary


# 兼容旧名
def fit_to_chunk(
    prepared: PreparedImage,
    chunk_max: int,
    **_kwargs,
) -> PreparedImage:
    """已弃用路径：仅当已编码图碰巧合格时通过；A2I1 超限请改用 prepare_inline。"""
    if prepared.payload_size <= chunk_max:
        return prepared
    raise ValueError(
        f"payload {prepared.payload_size} > {chunk_max}; use prepare_inline/prepare_cover"
    )
