"""`.ebook` 二进制常量与编解码辅助。"""

from __future__ import annotations

import struct
import zlib
from typing import BinaryIO

MAGIC = b"EBOK"
VERSION = 1

FLAG_HAS_COVER = 1 << 0
FLAG_IMG_A2I1 = 1 << 1
FLAG_ZLIB = 1 << 2
FLAG_HAS_TOC = 1 << 3  # 含原生目录（PDF Outline / EPUB nav·NCX）；无则设备显示「暂无目录」
FLAG_PAGE_IMAGES = 1 << 4  # 整页转图：阅读端全屏铺满显示区 (0,0)，无正文边距

BLOCK_TEXT = 0x01
BLOCK_IMAGE = 0x02

BF_ZLIB = 1 << 0

IMG_JPEG = 1
IMG_PNG = 2
IMG_A2I1 = 3
IMG_GRAY = 4

CTRL_PARA = 0x1E  # 段落分隔

HEADER_SIZE = 64
CHAPTER_ENTRY_SIZE = 32
BLOCK_HEADER_SIZE = 8
IMAGE_PAYLOAD_HEADER = 12

DEFAULT_CHUNK_MAX = 4096
# 432×720 A2I1 ImagePayload ≈ 39KB；与固件 ebook_document kMaxChunk 对齐
DEVICE_CHUNK_MAX = 49152
DEFAULT_FONT_PX = 25
DEFAULT_IMAGE_MODE = "a2i1"  # 产品固定；CLI 不再提供其它默认值

# 正文插图 / 扫描页默认最大框（大于则等比例缩小；小于不放大）
DEFAULT_IMAGE_MAX_W = 432
DEFAULT_IMAGE_MAX_H = 720

# 封面与正文插图分轨：封面一次读入详情，不占用翻页解压缓冲
DEFAULT_COVER_MAX_W = 240
DEFAULT_COVER_MAX_H = 320
COVER_MAX_PAYLOAD = 24 * 1024  # ImagePayload 总长上限（含 12B 头）

# 单章解压合计上限：转换拆章预算 / 固件硬拒（须保持 DEFAULT < DEVICE）
# 固件同步：main/reader/book_session.cc kMaxChapterUncompForPaginate
DEVICE_CHAPTER_MAX_UNCOMP = 384 * 1024
DEFAULT_CHAPTER_MAX_BYTES = 256 * 1024
if not (1024 <= DEFAULT_CHAPTER_MAX_BYTES < DEVICE_CHAPTER_MAX_UNCOMP):
    raise RuntimeError("DEFAULT_CHAPTER_MAX_BYTES out of range vs DEVICE_CHAPTER_MAX_UNCOMP")

# PDF 扫描页整页转图（对齐设备阅读区全宽 480；高度约状态栏+底栏下方可用高度）
DEFAULT_PDF_PAGE_SCALE = 1.5  # PyMuPDF Matrix 倍率（相对 72dpi）
DEFAULT_PDF_PAGE_MAX_W = 480
DEFAULT_PDF_PAGE_MAX_H = 720


def parse_size_box(spec: str) -> tuple[int, int]:
    """
    解析缩放参数：
      - \"200\"     → (200, 200)，最长边不超过 200（等比例）
      - \"200x280\" → (200, 280)，放入框内等比例（只缩小不放大）
      - \"orig\" / \"original\" / \"0x0\" → (0, 0)，不按最大框缩小（仍受 chunk 预算约束）
    """
    s = (spec or "").strip().lower().replace("*", "x").replace("×", "x")
    if not s:
        raise ValueError("empty size spec")
    if s in ("orig", "original", "native", "raw", "source", "0x0", "0"):
        return 0, 0
    if "x" in s:
        a, b = s.split("x", 1)
        w, h = int(a.strip()), int(b.strip())
    else:
        w = h = int(s)
    if w == 0 and h == 0:
        return 0, 0
    if w < 8 or h < 8 or w > 4096 or h > 4096:
        raise ValueError(f"size out of range: {w}x{h}")
    return w, h


# Header: 见 SPEC.md §3
# magic, version, flags, chapter_count, default_font_px, chunk_max,
# meta_off, meta_sz, index_off, index_sz, title_off, title_sz,
# data_off, data_sz, cover_off, cover_sz, crc32, reserved
HEADER_STRUCT = struct.Struct("<4sHHIHH12I")
assert HEADER_STRUCT.size == HEADER_SIZE

CHAPTER_STRUCT = struct.Struct("<III HH I I 8x")
assert CHAPTER_STRUCT.size == CHAPTER_ENTRY_SIZE

BLOCK_STRUCT = struct.Struct("<BBHHH")
assert BLOCK_STRUCT.size == BLOCK_HEADER_SIZE


def crc32_header(raw56: bytes) -> int:
    """对 Header 前 56 字节计算 CRC-32（zlib 兼容）。"""
    return zlib.crc32(raw56) & 0xFFFFFFFF


def pack_header(
    *,
    flags: int,
    chapter_count: int,
    default_font_px: int,
    chunk_max_uncomp: int,
    meta_offset: int,
    meta_size: int,
    index_offset: int,
    index_size: int,
    title_blob_offset: int,
    title_blob_size: int,
    data_offset: int,
    data_size: int,
    cover_offset: int,
    cover_size: int,
) -> bytes:
    body = HEADER_STRUCT.pack(
        MAGIC,
        VERSION,
        flags,
        chapter_count,
        default_font_px,
        chunk_max_uncomp,
        meta_offset,
        meta_size,
        index_offset,
        index_size,
        title_blob_offset,
        title_blob_size,
        data_offset,
        data_size,
        cover_offset,
        cover_size,
        0,  # crc placeholder
        0,  # reserved
    )
    crc = crc32_header(body[:56])
    return body[:56] + struct.pack("<I", crc) + body[60:]


def pack_block(type_: int, raw: bytes, compress: bool = True) -> bytes:
    if len(raw) > 0xFFFF:
        raise ValueError(f"block raw_size {len(raw)} exceeds u16")
    if compress:
        payload = zlib.compress(raw, level=6)
        bflags = BF_ZLIB
    else:
        payload = raw
        bflags = 0
    if len(payload) > 0xFFFF:
        raise ValueError(f"block comp_size {len(payload)} exceeds u16")
    hdr = BLOCK_STRUCT.pack(type_, bflags, len(payload), len(raw), 0)
    return hdr + payload


def pack_image_payload(
    fmt: int,
    width: int,
    height: int,
    data: bytes,
    stride: int = 0,
) -> bytes:
    if len(data) > 0xFFFFFFFF:
        raise ValueError("image too large")
    return (
        struct.pack("<BBHHHI", fmt, 0, width, height, stride, len(data)) + data
    )


def write_u16_str(buf: bytearray, text: str) -> None:
    data = text.encode("utf-8")
    if len(data) > 0xFFFF:
        raise ValueError("string too long for u16 length")
    buf += struct.pack("<H", len(data))
    buf += data


def read_exact(fp: BinaryIO, n: int) -> bytes:
    data = fp.read(n)
    if len(data) != n:
        raise EOFError(f"need {n} bytes, got {len(data)}")
    return data
