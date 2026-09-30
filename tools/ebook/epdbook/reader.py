"""桌面端 / 调试用 `.ebook` 解析（与固件 EbookDocument 语义对齐）。"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple, Union

from .format import (
    BF_ZLIB,
    BLOCK_HEADER_SIZE,
    BLOCK_IMAGE,
    BLOCK_STRUCT,
    BLOCK_TEXT,
    CHAPTER_ENTRY_SIZE,
    CHAPTER_STRUCT,
    CTRL_PARA,
    FLAG_HAS_COVER,
    FLAG_HAS_TOC,
    FLAG_IMG_A2I1,
    FLAG_PAGE_IMAGES,
    FLAG_ZLIB,
    HEADER_SIZE,
    HEADER_STRUCT,
    IMAGE_PAYLOAD_HEADER,
    IMG_A2I1,
    IMG_GRAY,
    IMG_JPEG,
    IMG_PNG,
    MAGIC,
    crc32_header,
    read_exact,
)


@dataclass
class ChapterInfo:
    index: int
    title: str
    data_offset: int
    data_length: int
    uncompressed_total: int
    block_count: int
    flags: int


@dataclass
class ContentPiece:
    """章内一项：文本段落或已解码图片字节。"""

    kind: str  # "text" | "image"
    text: str = ""
    image_fmt: int = 0
    width: int = 0
    height: int = 0
    stride: int = 0
    image_data: bytes = b""
    # 调试：该块在文件中的偏移
    block_offset: int = 0


@dataclass
class EbookMeta:
    title: str = ""
    author: str = ""
    lang: str = ""
    extra: str = ""


@dataclass
class EbookFile:
    path: Path
    version: int = 0
    flags: int = 0
    font_px: int = 25
    chunk_max: int = 4096
    meta: EbookMeta = field(default_factory=EbookMeta)
    chapters: List[ChapterInfo] = field(default_factory=list)
    cover_offset: int = 0
    cover_size: int = 0
    crc_ok: bool = False
    header_fields: dict = field(default_factory=dict)

    @property
    def has_cover(self) -> bool:
        return bool(self.flags & FLAG_HAS_COVER) and self.cover_offset > 0

    @property
    def book_id(self) -> str:
        """Metadata.extra.book_id；缺失为空串。"""
        from .meta_extra import book_id_from_extra

        return book_id_from_extra(self.meta.extra)

    @property
    def has_toc(self) -> bool:
        """Header FLAG_HAS_TOC：原生/启发式目录。按体积拆章不置此位。"""
        return bool(self.flags & FLAG_HAS_TOC)

    @property
    def prefer_a2i1(self) -> bool:
        return bool(self.flags & FLAG_IMG_A2I1)

    @property
    def page_images(self) -> bool:
        """Header FLAG_PAGE_IMAGES：整页转图书，阅读端全屏铺满。"""
        return bool(self.flags & FLAG_PAGE_IMAGES)


def open_ebook(path: Union[str, Path]) -> EbookFile:
    path = Path(path)
    with path.open("rb") as fp:
        raw = read_exact(fp, HEADER_SIZE)
        fields = HEADER_STRUCT.unpack(raw)
        if fields[0] != MAGIC:
            raise ValueError(f"bad magic {fields[0]!r}")
        if fields[1] != 1:
            raise ValueError(f"unsupported version {fields[1]}")

        book = EbookFile(
            path=path,
            version=fields[1],
            flags=fields[2],
            font_px=fields[4],
            chunk_max=fields[5],
            cover_offset=fields[14],
            cover_size=fields[15],
            crc_ok=crc32_header(raw[:56]) == fields[16],
            header_fields={
                "chapter_count": fields[3],
                "meta_offset": fields[6],
                "meta_size": fields[7],
                "index_offset": fields[8],
                "index_size": fields[9],
                "title_blob_offset": fields[10],
                "title_blob_size": fields[11],
                "data_offset": fields[12],
                "data_size": fields[13],
                "cover_offset": fields[14],
                "cover_size": fields[15],
                "zlib": bool(fields[2] & FLAG_ZLIB),
            },
        )

        fp.seek(fields[6])
        book.meta = _read_meta(fp)

        nch = fields[3]
        fp.seek(fields[8])
        for i in range(nch):
            ent = read_exact(fp, CHAPTER_ENTRY_SIZE)
            data_off, data_len, uncomp, bcnt, tlen, toff, fl = CHAPTER_STRUCT.unpack(ent)
            title = ""
            if tlen > 0:
                cur = fp.tell()
                fp.seek(toff)
                title = read_exact(fp, tlen).decode("utf-8", errors="replace")
                fp.seek(cur)
            if not title:
                title = f"第{i + 1}章"
            book.chapters.append(
                ChapterInfo(
                    index=i,
                    title=title,
                    data_offset=data_off,
                    data_length=data_len,
                    uncompressed_total=uncomp,
                    block_count=bcnt,
                    flags=fl,
                )
            )
    return book


def _read_u16_str(fp) -> str:
    (n,) = struct.unpack("<H", read_exact(fp, 2))
    if n == 0:
        return ""
    return read_exact(fp, n).decode("utf-8", errors="replace")


def _read_meta(fp) -> EbookMeta:
    return EbookMeta(
        title=_read_u16_str(fp),
        author=_read_u16_str(fp),
        lang=_read_u16_str(fp),
        extra=_read_u16_str(fp),
    )


def _inflate(comp: bytes, raw_size: int, zlib_flag: bool) -> bytes:
    if zlib_flag:
        raw = zlib.decompress(comp)
    else:
        raw = comp
    if len(raw) != raw_size:
        # 容忍 zlib 尾部差异时以声明为准截断/报错
        if len(raw) > raw_size:
            raw = raw[:raw_size]
        elif len(raw) < raw_size:
            raise ValueError(f"inflate size {len(raw)} != {raw_size}")
    return raw


def _read_block_at(fp, offset: int) -> Tuple[int, bytes, int]:
    """返回 (type, raw_payload, next_offset)。"""
    fp.seek(offset)
    hdr = read_exact(fp, BLOCK_HEADER_SIZE)
    type_, bflags, comp_size, raw_size, _ = BLOCK_STRUCT.unpack(hdr)
    comp = read_exact(fp, comp_size)
    raw = _inflate(comp, raw_size, bool(bflags & BF_ZLIB))
    next_off = offset + BLOCK_HEADER_SIZE + comp_size
    return type_, raw, next_off


def _parse_image_payload(raw: bytes) -> Tuple[int, int, int, int, bytes]:
    if len(raw) < IMAGE_PAYLOAD_HEADER:
        raise ValueError("image payload too short")
    fmt, _res, w, h, stride, data_len = struct.unpack("<BBHHHI", raw[:12])
    if 12 + data_len > len(raw):
        raise ValueError("image data truncated")
    return fmt, w, h, stride, raw[12 : 12 + data_len]


def _text_to_paragraphs(raw: bytes) -> List[str]:
    parts = raw.split(bytes([CTRL_PARA]))
    out: List[str] = []
    for p in parts:
        if not p:
            continue
        out.append(p.decode("utf-8", errors="replace"))
    return out


def pieces_have_content(pieces: List[ContentPiece]) -> bool:
    """是否有可展示内容（非空白文本或图片）。"""
    for p in pieces:
        if p.kind == "image":
            return True
        if p.kind == "text" and (p.text or "").strip():
            return True
    return False


def load_chapter(book: EbookFile, chapter_index: int) -> List[ContentPiece]:
    if chapter_index < 0 or chapter_index >= len(book.chapters):
        raise IndexError("chapter out of range")
    ch = book.chapters[chapter_index]
    pieces: List[ContentPiece] = []
    with book.path.open("rb") as fp:
        cursor = ch.data_offset
        end = ch.data_offset + ch.data_length
        for _ in range(ch.block_count):
            if cursor >= end:
                break
            block_off = cursor
            type_, raw, cursor = _read_block_at(fp, cursor)
            if type_ == BLOCK_TEXT:
                for para in _text_to_paragraphs(raw):
                    pieces.append(
                        ContentPiece(kind="text", text=para, block_offset=block_off)
                    )
            elif type_ == BLOCK_IMAGE:
                fmt, w, h, stride, data = _parse_image_payload(raw)
                pieces.append(
                    ContentPiece(
                        kind="image",
                        image_fmt=fmt,
                        width=w,
                        height=h,
                        stride=stride,
                        image_data=data,
                        block_offset=block_off,
                    )
                )
    return pieces


def load_cover_payload(book: EbookFile) -> Optional[ContentPiece]:
    if not book.has_cover or book.cover_size <= 0:
        return None
    with book.path.open("rb") as fp:
        fp.seek(book.cover_offset)
        raw = read_exact(fp, book.cover_size)
    fmt, w, h, stride, data = _parse_image_payload(raw)
    return ContentPiece(
        kind="image",
        image_fmt=fmt,
        width=w,
        height=h,
        stride=stride,
        image_data=data,
        block_offset=book.cover_offset,
    )


def decode_image_to_pil(piece: ContentPiece):
    """ContentPiece(image) → PIL Image（L 或 1）。"""
    from io import BytesIO

    from PIL import Image

    if piece.kind != "image":
        raise ValueError("not an image")
    data = piece.image_data
    fmt = piece.image_fmt
    if fmt == IMG_A2I1:
        return _a2i1_to_pil(data)
    if fmt == IMG_JPEG:
        return Image.open(BytesIO(data)).convert("RGB")
    if fmt == IMG_PNG:
        return Image.open(BytesIO(data)).convert("RGB")
    if fmt == IMG_GRAY:
        if data[:4] == b"EBGR" and len(data) >= 8:
            w = data[4] | (data[5] << 8)
            h = data[6] | (data[7] << 8)
            return Image.frombytes("L", (w, h), data[8 : 8 + w * h])
        return Image.frombytes("L", (piece.width, piece.height), data)
    raise ValueError(f"unknown image fmt {fmt}")


def _a2i1_to_pil(data: bytes):
    from PIL import Image

    if len(data) < 20 or data[:4] != b"A2I1":
        raise ValueError("not A2I1")
    w = data[4] | (data[5] << 8)
    h = data[6] | (data[7] << 8)
    stride = data[8] | (data[9] << 8)
    bits = data[20:]
    pixels = bytearray(w * h)
    for y in range(h):
        row = bits[y * stride : (y + 1) * stride]
        for x in range(w):
            white = (row[x >> 3] & (0x80 >> (x & 7))) != 0
            pixels[y * w + x] = 255 if white else 0
    return Image.frombytes("L", (w, h), bytes(pixels))
