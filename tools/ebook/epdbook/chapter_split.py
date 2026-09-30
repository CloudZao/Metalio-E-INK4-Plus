"""超大章按解压合计体积拆分，避免设备整章物化分页 OOM。

预算与写出侧 `uncompressed_total` 对齐（文本 payload + ImagePayload），
默认上限 `DEFAULT_CHAPTER_MAX_BYTES`(256KB) < 固件硬拒
`DEVICE_CHAPTER_MAX_UNCOMP`(384KB)。体积拆章不置 HAS_TOC。
"""

from __future__ import annotations

from typing import List, Sequence, Tuple

from .format import (
    DEFAULT_CHAPTER_MAX_BYTES,
    DEVICE_CHAPTER_MAX_UNCOMP,
    IMAGE_PAYLOAD_HEADER,
)
from .writer import ChapterDraft, ContentItem, chapter_draft_has_content, paragraphs_to_payload

__all__ = [
    "DEFAULT_CHAPTER_MAX_BYTES",
    "DEVICE_CHAPTER_MAX_UNCOMP",
    "split_oversized_chapters",
]


def _utf8_hard_split(text: str, max_bytes: int) -> List[str]:
    """单段超过上限时按 UTF-8 码点硬切（边界切开，解码必成功）。"""
    data = text.encode("utf-8")
    if len(data) <= max_bytes:
        return [text]
    if max_bytes < 4:
        raise ValueError("hard-split max_bytes too small")
    out: List[str] = []
    i = 0
    n = len(data)
    while i < n:
        end = min(i + max_bytes, n)
        if end < n:
            while end > i and (data[end] & 0xC0) == 0x80:
                end -= 1
            if end == i:
                # 极端：单码点 > max_bytes（理论上 UTF-8 ≤4B）；硬吃 max_bytes
                end = min(i + max_bytes, n)
        chunk = data[i:end].decode("utf-8")
        if chunk:
            out.append(chunk)
        i = end
    return out or [text]


def _item_uncomp_bytes(it: ContentItem) -> int:
    """估算写入后计入 chapter.uncompressed_total 的体积。"""
    if it.kind == "text":
        return len(paragraphs_to_payload(it.paragraphs))
    if it.kind == "image":
        # 与 writer.pack_image_payload 一致：12B 头 + 像素/压缩数据
        return IMAGE_PAYLOAD_HEADER + len(it.image_data or b"")
    return 0


def _chapter_uncomp_bytes(ch: ChapterDraft) -> int:
    return sum(_item_uncomp_bytes(it) for it in ch.items)


def _split_text_item(it: ContentItem, max_bytes: int) -> List[ContentItem]:
    """将过大的 text ContentItem 按段落（必要时硬切）拆成多个 ≤ max_bytes 的项。"""
    payload_len = len(paragraphs_to_payload(it.paragraphs))
    if payload_len <= max_bytes:
        return [it]

    parts: List[ContentItem] = []
    cur_paras: List[str] = []
    cur_bytes = 0
    sep = 1  # CTRL_PARA after each para in paragraphs_to_payload

    def flush() -> None:
        nonlocal cur_paras, cur_bytes
        if cur_paras:
            parts.append(ContentItem(kind="text", paragraphs=list(cur_paras)))
            cur_paras = []
            cur_bytes = 0

    for para in it.paragraphs:
        t = (para or "").replace("\r\n", "\n").replace("\r", "\n")
        if t == "":
            continue
        pb = t.encode("utf-8")
        need = len(pb) + sep
        if need > max_bytes:
            flush()
            for chunk in _utf8_hard_split(t, max(16, max_bytes - sep)):
                parts.append(ContentItem(kind="text", paragraphs=[chunk]))
            continue
        if cur_paras and cur_bytes + need > max_bytes:
            flush()
        cur_paras.append(t)
        cur_bytes += need
    flush()
    return parts or [it]


def _part_title(base: str, index: int, total: int) -> str:
    base = (base or "全文").strip() or "全文"
    if total <= 1:
        return base
    return f"{base} ({index}/{total})"


def split_oversized_chapters(
    chapters: Sequence[ChapterDraft],
    max_chapter_bytes: int = DEFAULT_CHAPTER_MAX_BYTES,
) -> Tuple[List[ChapterDraft], int]:
    """
    按解压合计体积拆分超大章。返回 (新章列表, 被拆分的原章数)。

    - 文本 + 图 ImagePayload 一并计入（对齐设备 uncompressed_total）
    - 优先在 ContentItem / 段落边界切开；单图不拆（单图已受 chunk_max 约束）
    - 标题：原名 (i/n)；不置 HAS_TOC
    - 空桶丢弃；拆后若原章内容全丢则保留原章并报错式跳过拆分
    """
    if max_chapter_bytes < 1024:
        raise ValueError("chapter_max_bytes too small")
    if max_chapter_bytes >= DEVICE_CHAPTER_MAX_UNCOMP:
        raise ValueError(
            f"chapter_max_bytes ({max_chapter_bytes}) must be < "
            f"DEVICE_CHAPTER_MAX_UNCOMP ({DEVICE_CHAPTER_MAX_UNCOMP})"
        )

    out: List[ChapterDraft] = []
    split_src = 0

    for ch in chapters:
        if _chapter_uncomp_bytes(ch) <= max_chapter_bytes:
            out.append(ch)
            continue

        # 先把过大 text item 拆开，再按累计体积装桶
        flat: List[ContentItem] = []
        for it in ch.items:
            if it.kind == "text":
                flat.extend(_split_text_item(it, max_chapter_bytes))
            else:
                flat.append(it)

        buckets: List[List[ContentItem]] = []
        cur: List[ContentItem] = []
        cur_bytes = 0

        def flush_bucket() -> None:
            nonlocal cur, cur_bytes
            if cur:
                buckets.append(cur)
                cur = []
                cur_bytes = 0

        for it in flat:
            ib = _item_uncomp_bytes(it)
            # 单图 / 已硬切文本仍可能 == 或略大于预算：独占一桶，不与其它内容合并
            if ib > max_chapter_bytes:
                flush_bucket()
                buckets.append([it])
                continue
            if cur and cur_bytes + ib > max_chapter_bytes:
                flush_bucket()
            cur.append(it)
            cur_bytes += ib
        flush_bucket()

        # 丢掉空内容桶；若全空则保守保留原章（避免静默丢书）
        parts: List[ChapterDraft] = []
        for items in buckets:
            draft = ChapterDraft(title="", items=items)
            if chapter_draft_has_content(draft):
                parts.append(draft)
        if not parts:
            out.append(ch)
            continue

        split_src += 1
        n = len(parts)
        base = ch.title or "全文"
        for i, part in enumerate(parts, start=1):
            part.title = _part_title(base, i, n)
            out.append(part)

    return out, split_src
