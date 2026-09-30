"""EPUB 提取：优先原生目录（nav / NCX / ebooklib toc），否则单章全文并标记无目录。

- 有 toc：按目录项标题分章，正文取对应 href 文档
- 无 toc：spine 全部合并为「全文」，has_toc=False（转换时提示）
- 封面：OPF meta name=cover / ITEM_COVER / guide 封面页 / id·文件名含 cover；与插图分轨，默认 A2I1
"""

from __future__ import annotations

import re
from html.parser import HTMLParser
from pathlib import Path
from typing import Any, Callable, List, Optional, Sequence, Tuple

from ..format import (
    COVER_MAX_PAYLOAD,
    DEFAULT_COVER_MAX_H,
    DEFAULT_COVER_MAX_W,
    DEFAULT_IMAGE_MAX_H,
    DEFAULT_IMAGE_MAX_W,
    DEFAULT_IMAGE_MODE,
)
from ..image_prep import BinarizeOptions, prepare_cover, prepare_inline
from ..writer import ChapterDraft, ContentItem
from . import ExtractedBook


class _HtmlToFlow(HTMLParser):
    """把 HTML 压成 (paragraphs, images_in_order) 流。"""

    BLOCK = {
        "p",
        "div",
        "h1",
        "h2",
        "h3",
        "h4",
        "h5",
        "h6",
        "li",
        "tr",
        "br",
        "section",
        "article",
    }

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.events: List[Tuple[str, object]] = []
        self._buf: List[str] = []
        self._skip = 0

    def handle_starttag(self, tag: str, attrs) -> None:
        tag = tag.lower()
        if tag in ("script", "style", "svg"):
            self._skip += 1
            return
        if self._skip:
            return
        if tag == "br":
            self._flush_para()
            return
        if tag == "img":
            self._flush_para()
            src = dict(attrs).get("src") or dict(attrs).get("xlink:href") or ""
            if src:
                self.events.append(("img", src))
            return
        if tag in self.BLOCK:
            self._flush_para()

    def handle_endtag(self, tag: str) -> None:
        tag = tag.lower()
        if tag in ("script", "style", "svg"):
            self._skip = max(0, self._skip - 1)
            return
        if self._skip:
            return
        if tag in self.BLOCK:
            self._flush_para()

    def handle_data(self, data: str) -> None:
        if self._skip:
            return
        if data:
            self._buf.append(data)

    def _flush_para(self) -> None:
        text = "".join(self._buf)
        self._buf.clear()
        text = re.sub(r"\s+", " ", text).strip()
        if text:
            self.events.append(("text", text))

    def close(self) -> None:
        self._flush_para()
        super().close()


def _resolve_href(base: str, href: str) -> str:
    href = (href or "").split("#", 1)[0].strip()
    if not href:
        return ""
    if href.startswith("/"):
        return href.lstrip("/")
    parts = base.split("/")
    dirs = parts[:-1]
    for seg in href.split("/"):
        if seg == "..":
            if dirs:
                dirs.pop()
        elif seg == "." or seg == "":
            continue
        else:
            dirs.append(seg)
    return "/".join(dirs)


def _prepared_to_item(prep) -> ContentItem:
    return ContentItem(
        kind="image",
        image_fmt=prep.fmt,
        width=prep.width,
        height=prep.height,
        stride=prep.stride,
        image_data=prep.data,
    )


def _try_cover_from_bytes(
    data: bytes,
    *,
    image_mode: str,
    cover_max_w: int,
    cover_max_h: int,
    cover_max_payload: int,
    binarize: Optional[BinarizeOptions] = None,
) -> Optional[ContentItem]:
    if not data:
        return None
    prep = prepare_cover(
        data,
        mode=image_mode,
        max_width=cover_max_w,
        max_height=cover_max_h,
        max_payload=cover_max_payload,
        binarize=binarize,
    )
    return _prepared_to_item(prep) if prep else None


def _opf_cover_id(book) -> Optional[str]:
    """读 OPF <meta name="cover" content="id"/>。"""
    for row in book.get_metadata("OPF", "meta") or []:
        # ebooklib: (None, {'name': 'cover', 'content': 'cover'})
        if not isinstance(row, (list, tuple)) or len(row) < 2:
            continue
        attrs = row[1] if isinstance(row[1], dict) else {}
        name = (attrs.get("name") or attrs.get("property") or "").lower()
        if name == "cover":
            cid = (attrs.get("content") or attrs.get("id") or "").strip()
            if cid:
                return cid
    return None


def _guide_cover_href(book) -> Optional[str]:
    for ent in getattr(book, "guide", None) or []:
        if not isinstance(ent, dict):
            continue
        if (ent.get("type") or "").lower() == "cover":
            href = (ent.get("href") or "").strip()
            if href:
                return href.split("#", 1)[0]
    return None


def _first_img_href_in_html(html: str) -> Optional[str]:
    # 普通 <img> 或封面页常见的 SVG <image xlink:href=...>
    m = re.search(
        r"""<(?:img|image)\b[^>]+(?:(?:xlink:)?href|src)\s*=\s*['"]([^'"]+)['"]""",
        html,
        re.I,
    )
    return m.group(1).strip() if m else None


def _find_epub_cover_bytes(book, by_name: dict) -> Optional[bytes]:
    """按优先级找封面原图字节。

    1. ebooklib ITEM_COVER
    2. OPF meta name=cover → item id（常见：id=cover，文件名却是 00001.jpeg）
    3. guide type=cover 页面里的首张 img/svg:image
    4. item id / 文件名含 cover
    """
    import ebooklib

    for it in book.get_items():
        if it.get_type() == ebooklib.ITEM_COVER:
            data = it.get_content() or b""
            if data:
                return data

    cid = _opf_cover_id(book)
    if cid:
        it = book.get_item_with_id(cid)
        if it is not None:
            data = it.get_content() or b""
            if data:
                return data

    href = _guide_cover_href(book)
    if href:
        doc = _find_doc(by_name, href)
        if doc is not None:
            html = (doc.get_content() or b"").decode("utf-8", errors="replace")
            img_href = _first_img_href_in_html(html)
            if img_href:
                img = _find_doc(by_name, _resolve_href(doc.get_name(), img_href))
                if img is not None:
                    data = img.get_content() or b""
                    if data:
                        return data

    for it in book.get_items_of_type(ebooklib.ITEM_IMAGE):
        name = (it.get_name() or "").lower()
        iid = (it.get_id() or "").lower()
        if "cover" in name or "cover" in iid or iid in ("cover-image", "coverimage"):
            data = it.get_content() or b""
            if data:
                return data
    return None


def _flatten_epub_toc(nodes: Any, out: List[Tuple[str, str]]) -> None:
    """摊平 ebooklib toc → [(title, href), ...]。"""
    if nodes is None:
        return
    if not isinstance(nodes, (list, tuple)):
        nodes = [nodes]
    for node in nodes:
        if node is None:
            continue
        # (Section|Link, [children])
        if isinstance(node, tuple) and len(node) >= 1:
            _flatten_epub_toc(node[0], out)
            if len(node) >= 2:
                _flatten_epub_toc(node[1], out)
            continue
        if isinstance(node, list):
            _flatten_epub_toc(node, out)
            continue
        title = (getattr(node, "title", None) or "").strip()
        href = (getattr(node, "href", None) or "").strip()
        if title or href:
            out.append((title or "未命名", href))


def _find_doc(by_name: dict, href: str):
    if not href:
        return None
    if href in by_name:
        return by_name[href]
    base = href.split("/")[-1]
    for name, cand in by_name.items():
        if name == href or name.endswith("/" + base) or name.endswith(base):
            return cand
    return None


def _events_to_items(
    events: List[Tuple[str, object]],
    *,
    doc_name: str,
    by_name: dict,
    include_images: bool,
    image_mode: str,
    max_width: int,
    max_height: int,
    chunk_max: int,
    binarize: Optional[BinarizeOptions] = None,
) -> List[ContentItem]:
    content_items: List[ContentItem] = []
    para_buf: List[str] = []

    def flush() -> None:
        nonlocal para_buf
        if para_buf:
            content_items.append(ContentItem(kind="text", paragraphs=list(para_buf)))
            para_buf = []

    for kind, val in events:
        if kind == "text":
            para_buf.append(str(val))
        elif kind == "img" and include_images:
            flush()
            href = _resolve_href(doc_name, str(val))
            img_it = _find_doc(by_name, href)
            if img_it is None:
                continue
            prep = prepare_inline(
                img_it.get_content(),
                mode=image_mode,
                max_width=max_width,
                max_height=max_height,
                chunk_max=chunk_max,
                binarize=binarize,
            )
            if prep:
                content_items.append(_prepared_to_item(prep))
    flush()
    return content_items


def _parse_document_item(
    it,
    *,
    by_name: dict,
    include_images: bool,
    image_mode: str,
    max_width: int,
    max_height: int,
    chunk_max: int,
    binarize: Optional[BinarizeOptions] = None,
) -> List[ContentItem]:
    html = it.get_content().decode("utf-8", errors="replace")
    parser = _HtmlToFlow()
    parser.feed(html)
    parser.close()
    return _events_to_items(
        parser.events,
        doc_name=it.get_name(),
        by_name=by_name,
        include_images=include_images,
        image_mode=image_mode,
        max_width=max_width,
        max_height=max_height,
        chunk_max=chunk_max,
        binarize=binarize,
    )


def extract_epub(
    path: Path,
    *,
    image_mode: str = DEFAULT_IMAGE_MODE,
    max_width: int = DEFAULT_IMAGE_MAX_W,
    max_height: int = DEFAULT_IMAGE_MAX_H,
    chunk_max: int = 4096,
    include_images: bool = True,
    chapter_regex: Optional[Sequence[str]] = None,
    include_cover: bool = True,
    cover_max_width: int = DEFAULT_COVER_MAX_W,
    cover_max_height: int = DEFAULT_COVER_MAX_H,
    cover_max_payload: int = COVER_MAX_PAYLOAD,
    binarize: Optional[BinarizeOptions] = None,
    progress: Optional[Callable[[int, str], None]] = None,
) -> ExtractedBook:
    try:
        import ebooklib
        from ebooklib import epub
    except ImportError as e:
        raise RuntimeError("需要 ebooklib：pip install ebooklib") from e

    def _prog(pct: int, msg: str) -> None:
        if progress is None:
            return
        try:
            progress(max(0, min(100, int(pct))), msg)
        except Exception:
            pass

    _ = chapter_regex  # EPUB 不用启发式切章

    _prog(8, "读取 EPUB…")
    book = epub.read_epub(str(path))
    title = ""
    author = ""
    if book.get_metadata("DC", "title"):
        title = book.get_metadata("DC", "title")[0][0]
    if book.get_metadata("DC", "creator"):
        author = book.get_metadata("DC", "creator")[0][0]
    title = title or path.stem

    items = {it.get_id(): it for it in book.get_items()}
    by_name = {it.get_name(): it for it in book.get_items()}

    cover_item: Optional[ContentItem] = None
    if include_cover:
        _prog(12, "提取封面…")
        raw_cover = _find_epub_cover_bytes(book, by_name)
        if raw_cover:
            cover_item = _try_cover_from_bytes(
                raw_cover,
                image_mode=image_mode,
                cover_max_w=cover_max_width,
                cover_max_h=cover_max_height,
                cover_max_payload=cover_max_payload,
                binarize=binarize,
            )
    common_parse = dict(
        by_name=by_name,
        include_images=include_images,
        image_mode=image_mode,
        max_width=max_width,
        max_height=max_height,
        chunk_max=chunk_max,
        binarize=binarize,
    )

    # —— 原生目录 ——
    toc_links: List[Tuple[str, str]] = []
    _flatten_epub_toc(getattr(book, "toc", None), toc_links)
    # 去掉无 href 的纯 Section 标题（无法定位正文）
    toc_links = [(t, h) for t, h in toc_links if h]

    chapters: List[ChapterDraft] = []
    has_toc = False
    used_names: set = set()

    if len(toc_links) >= 1:
        has_toc = True
        n = max(1, len(toc_links))
        for i, (ch_title, href) in enumerate(toc_links):
            name = _resolve_href("", href) if "/" not in href else href.split("#", 1)[0]
            # href 相对 OPF；ebooklib get_name 已是包内路径
            name = href.split("#", 1)[0]
            it = _find_doc(by_name, name)
            if it is None or it.get_type() != ebooklib.ITEM_DOCUMENT:
                chapters.append(ChapterDraft(title=ch_title or "未命名", items=[]))
                continue
            doc_name = it.get_name()
            if doc_name in used_names:
                # 同文件多个书签：只保留标题章，避免正文重复
                chapters.append(ChapterDraft(title=ch_title or "未命名", items=[]))
                continue
            used_names.add(doc_name)
            items_out = _parse_document_item(it, **common_parse)
            chapters.append(ChapterDraft(title=ch_title or "未命名", items=items_out))
            if i % max(1, n // 25) == 0 or i + 1 == n:
                _prog(15 + int(55 * (i + 1) / n), f"解析章节 {i + 1}/{n}")

        # 未被目录引用的 spine 文档 → 附录
        spine_ids = [s[0] for s in book.spine]
        appendix: List[ContentItem] = []
        for sid in spine_ids:
            it = items.get(sid)
            if it is None or it.get_type() != ebooklib.ITEM_DOCUMENT:
                continue
            if it.get_name() in used_names:
                continue
            appendix.extend(_parse_document_item(it, **common_parse))
        if appendix:
            chapters.append(ChapterDraft(title="附录", items=appendix))
    else:
        # 无原生目录：合并 spine 为全文
        has_toc = False
        merged: List[ContentItem] = []
        for sid in [s[0] for s in book.spine]:
            it = items.get(sid)
            if it is None or it.get_type() != ebooklib.ITEM_DOCUMENT:
                continue
            merged.extend(_parse_document_item(it, **common_parse))
        chapters = [ChapterDraft(title="全文", items=merged)]

    if not chapters:
        chapters = [ChapterDraft(title="全文", items=[])]
        has_toc = False

    _prog(74, "EPUB 提取完成")
    return ExtractedBook(
        title=title,
        author=author,
        chapters=chapters,
        cover=cover_item,
        source_format="epub",
        has_toc=has_toc,
    )


# 供 mobi 复用正文插图路径
def _image_to_item(
    data: bytes,
    *,
    image_mode: str,
    max_width: int,
    max_height: int,
    chunk_max: int,
    binarize: Optional[BinarizeOptions] = None,
) -> ContentItem:
    prep = prepare_inline(
        data,
        mode=image_mode,
        max_width=max_width,
        max_height=max_height,
        chunk_max=chunk_max,
        binarize=binarize,
    )
    if prep is None:
        raise ValueError("inline image skipped (over budget or decode fail)")
    return _prepared_to_item(prep)
