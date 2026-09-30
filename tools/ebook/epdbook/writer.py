"""将章节草稿序列化为 .ebook 文件。"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Sequence, Union

from .format import (
    BLOCK_IMAGE,
    BLOCK_TEXT,
    CHAPTER_ENTRY_SIZE,
    CHAPTER_STRUCT,
    COVER_MAX_PAYLOAD,
    CTRL_PARA,
    DEFAULT_CHUNK_MAX,
    DEFAULT_FONT_PX,
    FLAG_HAS_COVER,
    FLAG_HAS_TOC,
    FLAG_IMG_A2I1,
    FLAG_PAGE_IMAGES,
    FLAG_ZLIB,
    HEADER_SIZE,
    pack_block,
    pack_header,
    pack_image_payload,
    write_u16_str,
)


@dataclass
class ContentItem:
    """章内一项：文本段落列表，或一张图。"""

    kind: str  # "text" | "image"
    paragraphs: List[str] = field(default_factory=list)
    image_fmt: int = 0
    width: int = 0
    height: int = 0
    stride: int = 0
    image_data: bytes = b""


@dataclass
class ChapterDraft:
    title: str
    items: List[ContentItem] = field(default_factory=list)


def chapter_draft_has_content(ch: ChapterDraft) -> bool:
    """章内至少有一段非空白文本，或一张图。"""
    for it in ch.items:
        if it.kind == "image" and it.image_data:
            return True
        if it.kind == "text":
            for p in it.paragraphs:
                if p and str(p).strip():
                    return True
    return False


def _utf8_safe_chunks(data: bytes, max_size: int) -> List[bytes]:
    """按 UTF-8 码点边界切块，优先在段落分隔符处断开。"""
    if max_size < 4:
        raise ValueError("chunk_max_uncomp too small")
    out: List[bytes] = []
    i = 0
    n = len(data)
    while i < n:
        end = min(i + max_size, n)
        if end < n:
            # 优先回退到最近的段落符
            para = data.rfind(bytes([CTRL_PARA]), i, end)
            if para >= i + max_size // 4:
                end = para + 1
            else:
                # 回退到 UTF-8 首字节
                while end > i and (data[end] & 0xC0) == 0x80:
                    end -= 1
                if end == i:
                    end = min(i + max_size, n)
        out.append(data[i:end])
        i = end
    return out


def paragraphs_to_payload(paragraphs: Sequence[str]) -> bytes:
    """段之间 0x1E；段内允许保留换行与空格（不做 strip / 空白折叠）。"""
    parts: List[bytes] = []
    for p in paragraphs:
        t = (p or "").replace("\r\n", "\n").replace("\r", "\n")
        # 允许仅空白的段？跳过完全空串，但保留含空格/换行的内容
        if t == "":
            continue
        parts.append(t.encode("utf-8"))
    if not parts:
        return b""
    sep = bytes([CTRL_PARA])
    return sep.join(parts) + sep


class EbookWriter:
    def __init__(
        self,
        *,
        title: str = "",
        author: str = "",
        lang: str = "zh",
        extra: str = "",
        default_font_px: int = DEFAULT_FONT_PX,
        chunk_max_uncomp: int = DEFAULT_CHUNK_MAX,
        prefer_a2i1: bool = False,
        has_toc: bool = False,
        page_images: bool = False,
    ) -> None:
        self.title = title
        self.author = author
        self.lang = lang
        self.extra = extra
        self.default_font_px = default_font_px
        self.chunk_max_uncomp = chunk_max_uncomp
        self.prefer_a2i1 = prefer_a2i1
        self.has_toc = has_toc
        self.page_images = page_images
        self.chapters: List[ChapterDraft] = []
        self.cover_payload: Optional[bytes] = None

    def add_chapter(self, chapter: ChapterDraft) -> None:
        self.chapters.append(chapter)

    def set_cover_image(
        self,
        fmt: int,
        width: int,
        height: int,
        data: bytes,
        stride: int = 0,
        *,
        max_payload: int = COVER_MAX_PAYLOAD,
    ) -> None:
        """写入独立封面。预算用 COVER_MAX_PAYLOAD，与正文 chunk_max 无关。"""
        payload = pack_image_payload(fmt, width, height, data, stride)
        if len(payload) > max_payload:
            raise ValueError(
                f"cover ImagePayload {len(payload)} > cover_max {max_payload}; rescale first"
            )
        self.cover_payload = payload

    def clear_cover(self) -> None:
        self.cover_payload = None

    def _build_text_blocks(self, paragraphs: Sequence[str]) -> List[bytes]:
        raw = paragraphs_to_payload(paragraphs)
        if not raw:
            return []
        chunks = _utf8_safe_chunks(raw, self.chunk_max_uncomp)
        return [pack_block(BLOCK_TEXT, c, compress=True) for c in chunks]

    def _build_image_block(self, item: ContentItem) -> bytes:
        payload = pack_image_payload(
            item.image_fmt,
            item.width,
            item.height,
            item.image_data,
            item.stride,
        )
        if len(payload) > self.chunk_max_uncomp:
            raise ValueError(
                f"image payload {len(payload)} > chunk_max_uncomp "
                f"{self.chunk_max_uncomp}; use image_prep hooks to rescale"
            )
        # 小图可压可不压；zlib 对已压缩 JPEG 收益有限，仍统一压以简化设备路径
        return pack_block(BLOCK_IMAGE, payload, compress=True)

    def write(self, path: Union[str, Path]) -> None:
        kept = [ch for ch in self.chapters if chapter_draft_has_content(ch)]
        if not kept:
            raise ValueError("no non-empty chapters")
        self.chapters = kept

        meta = bytearray()
        write_u16_str(meta, self.title or "Untitled")
        write_u16_str(meta, self.author or "")
        write_u16_str(meta, self.lang or "")
        write_u16_str(meta, self.extra or "")

        # 先序列化每章的块，再回填偏移
        chapter_blobs: List[bytes] = []
        chapter_meta: List[dict] = []
        title_blob = bytearray()

        for ch in self.chapters:
            blocks: List[bytes] = []
            uncomp_total = 0
            has_image = False
            pending_paras: List[str] = []

            def flush_text() -> None:
                nonlocal pending_paras, uncomp_total
                if not pending_paras:
                    return
                for blk in self._build_text_blocks(pending_paras):
                    blocks.append(blk)
                    # raw_size 在块头 offset 4
                    uncomp_total += int.from_bytes(blk[4:6], "little")
                pending_paras = []

            for item in ch.items:
                if item.kind == "text":
                    pending_paras.extend(item.paragraphs)
                elif item.kind == "image":
                    flush_text()
                    has_image = True
                    blk = self._build_image_block(item)
                    blocks.append(blk)
                    uncomp_total += int.from_bytes(blk[4:6], "little")
                else:
                    raise ValueError(f"unknown item kind: {item.kind}")
            flush_text()

            if not blocks:
                # 理论上已被 chapter_draft_has_content 过滤；防御跳过
                continue

            blob = b"".join(blocks)
            title_utf8 = (ch.title or f"Chapter {len(chapter_meta)+1}").encode(
                "utf-8"
            )
            if len(title_utf8) > 0xFFFF:
                title_utf8 = title_utf8[:0xFFFF]
            title_off_in_blob = len(title_blob)
            title_blob += title_utf8

            chapter_blobs.append(blob)
            chapter_meta.append(
                {
                    "block_count": len(blocks),
                    "data_length": len(blob),
                    "uncompressed_total": uncomp_total,
                    "title_len": len(title_utf8),
                    "title_off_in_blob": title_off_in_blob,
                    "flags": 1 if has_image else 0,
                }
            )

        if not chapter_meta:
            raise ValueError("no non-empty chapters")

        meta_offset = HEADER_SIZE
        meta_size = len(meta)
        index_offset = meta_offset + meta_size
        index_size = len(chapter_meta) * CHAPTER_ENTRY_SIZE
        title_blob_offset = index_offset + index_size
        title_blob_size = len(title_blob)
        data_offset = title_blob_offset + title_blob_size

        # 计算每章 data_offset
        cursor = data_offset
        index_bin = bytearray()
        for cm in chapter_meta:
            index_bin += CHAPTER_STRUCT.pack(
                cursor,
                cm["data_length"],
                cm["uncompressed_total"],
                cm["block_count"],
                cm["title_len"],
                title_blob_offset + cm["title_off_in_blob"],
                cm["flags"],
            )
            cursor += cm["data_length"]

        data_blob = b"".join(chapter_blobs)
        data_size = len(data_blob)

        cover_offset = 0
        cover_size = 0
        cover_bin = b""
        if self.cover_payload:
            cover_offset = data_offset + data_size
            cover_size = len(self.cover_payload)
            cover_bin = self.cover_payload

        flags = FLAG_ZLIB
        if cover_bin:
            flags |= FLAG_HAS_COVER
        if self.prefer_a2i1:
            flags |= FLAG_IMG_A2I1
        if self.has_toc:
            flags |= FLAG_HAS_TOC
        if self.page_images:
            flags |= FLAG_PAGE_IMAGES

        header = pack_header(
            flags=flags,
            chapter_count=len(chapter_meta),
            default_font_px=self.default_font_px,
            chunk_max_uncomp=self.chunk_max_uncomp,
            meta_offset=meta_offset,
            meta_size=meta_size,
            index_offset=index_offset,
            index_size=index_size,
            title_blob_offset=title_blob_offset,
            title_blob_size=title_blob_size,
            data_offset=data_offset,
            data_size=data_size,
            cover_offset=cover_offset,
            cover_size=cover_size,
        )

        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("wb") as fp:
            fp.write(header)
            fp.write(meta)
            fp.write(index_bin)
            fp.write(title_blob)
            fp.write(data_blob)
            if cover_bin:
                fp.write(cover_bin)
