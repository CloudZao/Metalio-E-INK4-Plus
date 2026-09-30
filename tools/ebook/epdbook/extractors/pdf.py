"""PDF 提取：只用原生书签（Outline/TOC）；无则单章全文。

章节策略：
1. PDF 原生书签 get_toc() → 按页码切章（标题即书签名），has_toc=True
2. 无书签 → 单章「全文」，has_toc=False（转换时提示）
3. 仅 --force-heuristic 时才启用正则/字号启发式（仍不置 has_toc）
4. pages_as_images：扫描件整页光栅化 → 每页一张图（可按 TOC 归章）

封面与正文插图分轨；封面不受 --no-images 影响。
"""

from __future__ import annotations

from pathlib import Path
from typing import Callable, List, Optional, Sequence, Tuple

from ..chapter_detect import (
    lines_to_paragraphs,
    pdf_font_size_chapter_breaks,
    split_lines_to_chapters,
)
from ..format import (
    COVER_MAX_PAYLOAD,
    DEFAULT_COVER_MAX_H,
    DEFAULT_COVER_MAX_W,
    DEFAULT_IMAGE_MAX_H,
    DEFAULT_IMAGE_MAX_W,
    DEFAULT_IMAGE_MODE,
    DEFAULT_PDF_PAGE_MAX_H,
    DEFAULT_PDF_PAGE_MAX_W,
    DEFAULT_PDF_PAGE_SCALE,
)
from ..image_prep import BinarizeOptions, prepare_cover, prepare_inline
from ..writer import ChapterDraft, ContentItem
from . import ExtractedBook

ProgressCb = Callable[[int, str], None]


def _prog(progress: Optional[ProgressCb], pct: int, msg: str) -> None:
    if progress is None:
        return
    try:
        progress(max(0, min(100, int(pct))), msg)
    except Exception:
        pass


def _pix_png(doc, xref) -> Optional[bytes]:
    try:
        import fitz
    except ImportError:
        return None
    try:
        pix = fitz.Pixmap(doc, xref)
        if pix.n >= 5:
            pix = fitz.Pixmap(fitz.csRGB, pix)
        return pix.tobytes("png")
    except Exception:
        return None


def _extract_pdf_cover_bytes(doc) -> Optional[bytes]:
    """只负责拿到原始封面字节；编码交给 prepare_cover。"""
    import fitz

    if doc.page_count < 1:
        return None
    page0 = doc[0]

    best: Optional[Tuple[int, bytes]] = None
    for img in page0.get_images(full=True):
        xref = img[0]
        raw = _pix_png(doc, xref)
        if not raw:
            continue
        try:
            w = int(img[2]) if len(img) > 2 else 0
            h = int(img[3]) if len(img) > 3 else 0
            area = max(w * h, 1)
        except Exception:
            area = len(raw)
        if best is None or area > best[0]:
            best = (area, raw)
    if best is not None:
        return best[1]

    try:
        mat = fitz.Matrix(1.5, 1.5)
        pix = page0.get_pixmap(matrix=mat, alpha=False)
        return pix.tobytes("png")
    except Exception:
        return None


def _clean_toc_title(title: str) -> str:
    t = (title or "").replace("\r", " ").replace("\u3000", " ")
    t = " ".join(t.split())
    return t[:80] if t else ""


def _toc_entries(
    doc,
    *,
    max_level: Optional[int],
) -> List[Tuple[str, int]]:
    """
    返回 [(title, page0), ...]，page0 为 0-based。
    max_level=None 表示保留全部层级；=1 仅一级书签。
    """
    try:
        raw = doc.get_toc(simple=True) or []
    except Exception:
        return []
    out: List[Tuple[str, int]] = []
    n = doc.page_count
    for row in raw:
        if not row or len(row) < 3:
            continue
        level = int(row[0])
        title = _clean_toc_title(str(row[1]))
        page1 = int(row[2])
        if max_level is not None and level > max_level:
            continue
        if not title:
            continue
        page0 = max(0, min(n - 1, page1 - 1))
        out.append((title, page0))
    return out


def _chapters_from_pdf_toc(
    page_lines: List[List[str]],
    toc: List[Tuple[str, int]],
) -> List[Tuple[str, List[str], Tuple[int, int]]]:
    """
    按书签页码切章。
    返回 [(title, body_lines, (page_begin, page_end)), ...]，page_end 不含。
    同页多个书签：靠前的章正文为空（仅标题），正文归到最后一个同页书签起的区间。
    """
    if not toc or not page_lines:
        return []
    n = len(page_lines)
    chapters: List[Tuple[str, List[str], Tuple[int, int]]] = []

    # 首页书签之前的内容 → 前言
    first_page = toc[0][1]
    if first_page > 0:
        preface: List[str] = []
        for p in range(0, first_page):
            preface.extend(page_lines[p])
        if any(x.strip() for x in preface):
            chapters.append(("前言", preface, (0, first_page)))

    for i, (title, start) in enumerate(toc):
        end = toc[i + 1][1] if i + 1 < len(toc) else n
        if end < start:
            end = start
        body: List[str] = []
        # 下一书签也在本页 → 本章不独占正文，避免重复整页
        if end == start and i + 1 < len(toc):
            body = []
        else:
            for p in range(start, min(end, n)):
                body.extend(page_lines[p])
        chapters.append((title, body, (start, end if end > start else start)))

    return chapters


def _page_text_and_images(
    doc,
    *,
    include_images: bool,
    progress: Optional[ProgressCb] = None,
) -> Tuple[List[List[str]], List[List[bytes]], List[Tuple[float, str]], List[str]]:
    """逐页收集行、图、字号 span、以及扁平 all_lines。"""
    page_lines: List[List[str]] = []
    page_images: List[List[bytes]] = []
    span_sizes: List[Tuple[float, str]] = []
    all_lines: List[str] = []
    n = max(1, doc.page_count)

    for pi, page in enumerate(doc):
        lines: List[str] = []
        page_images.append([])
        blocks = page.get_text("dict").get("blocks", [])
        for block in blocks:
            if block.get("type") != 0:
                continue
            block_lines: List[str] = []
            for line in block.get("lines", []):
                parts: List[str] = []
                for span in line.get("spans", []):
                    t = span.get("text") or ""
                    sz = float(span.get("size") or 0)
                    # 保留 span 内空格；不做 split/strip
                    parts.append(t)
                    if t.strip():
                        span_sizes.append((sz, t.strip()))
                # 行内各 span 直接拼接（PDF 已在 span 文本里带空格）
                line_text = "".join(parts).replace("\u3000", "  ")
                if line_text.strip() == "" and not parts:
                    continue
                block_lines.append(line_text)
            if not block_lines:
                continue
            # 不同 text block 之间插入空行，近似段落间距（不丢行内空白）
            if lines:
                lines.append("")
                all_lines.append("")
            lines.extend(block_lines)
            all_lines.extend(block_lines)
        page_lines.append(lines)
        if include_images:
            for img in page.get_images(full=True):
                raw = _pix_png(doc, img[0])
                if raw:
                    page_images[-1].append(raw)
        if pi % max(1, n // 30) == 0 or pi + 1 == n:
            _prog(progress, 8 + int(55 * (pi + 1) / n), f"提取文字 {pi + 1}/{n} 页")

    return page_lines, page_images, span_sizes, all_lines


def _page_to_png(page, scale: float) -> Optional[bytes]:
    """整页光栅化为 PNG 字节。"""
    try:
        import fitz
    except ImportError:
        return None
    try:
        scale = max(0.25, min(float(scale), 4.0))
        mat = fitz.Matrix(scale, scale)
        pix = page.get_pixmap(matrix=mat, alpha=False)
        return pix.tobytes("png")
    except Exception:
        return None


def _prep_page_item(
    png: bytes,
    *,
    image_mode: str,
    page_max_width: int,
    page_max_height: int,
    chunk_max: int,
    binarize: Optional[BinarizeOptions] = None,
) -> Optional[ContentItem]:
    prep = prepare_inline(
        png,
        mode=image_mode,
        max_width=page_max_width,
        max_height=page_max_height,
        chunk_max=chunk_max,
        binarize=binarize,
    )
    if not prep:
        return None
    return ContentItem(
        kind="image",
        image_fmt=prep.fmt,
        width=prep.width,
        height=prep.height,
        stride=prep.stride,
        image_data=prep.data,
    )


def _extract_pdf_as_page_images(
    doc,
    *,
    title: str,
    author: str,
    use_pdf_toc: bool,
    pdf_toc_max_level: Optional[int],
    no_split: bool,
    image_mode: str,
    chunk_max: int,
    include_cover: bool,
    cover_max_width: int,
    cover_max_height: int,
    cover_max_payload: int,
    page_scale: float,
    page_max_width: int,
    page_max_height: int,
    binarize: Optional[BinarizeOptions] = None,
    progress: Optional[ProgressCb] = None,
) -> ExtractedBook:
    """扫描 PDF：每页一张图；有 TOC 则按书签页码归章，否则每页一章或全书一章。"""
    n = doc.page_count
    page_pngs: List[Optional[bytes]] = []
    for i in range(n):
        page_pngs.append(_page_to_png(doc[i], page_scale))
        if i % max(1, n // 40) == 0 or i + 1 == n:
            _prog(progress, 8 + int(50 * (i + 1) / max(n, 1)), f"渲染第 {i + 1}/{n} 页")

    cover: Optional[ContentItem] = None
    if include_cover and page_pngs and page_pngs[0]:
        _prog(progress, 60, "生成封面…")
        prep = prepare_cover(
            page_pngs[0],
            mode=image_mode,
            max_width=cover_max_width,
            max_height=cover_max_height,
            max_payload=cover_max_payload,
            binarize=binarize,
        )
        if prep:
            cover = ContentItem(
                kind="image",
                image_fmt=prep.fmt,
                width=prep.width,
                height=prep.height,
                stride=prep.stride,
                image_data=prep.data,
            )

    encoded = 0

    def page_item(pi: int) -> Optional[ContentItem]:
        nonlocal encoded
        raw = page_pngs[pi] if 0 <= pi < len(page_pngs) else None
        if not raw:
            return None
        it = _prep_page_item(
            raw,
            image_mode=image_mode,
            page_max_width=page_max_width,
            page_max_height=page_max_height,
            chunk_max=chunk_max,
            binarize=binarize,
        )
        encoded += 1
        if encoded % max(1, n // 30) == 0 or encoded >= n:
            _prog(progress, 62 + int(12 * encoded / max(n, 1)), f"编码页图 {encoded}/{n}")
        return it

    toc_used = False
    chapters: List[ChapterDraft] = []

    toc = _toc_entries(doc, max_level=pdf_toc_max_level) if (use_pdf_toc and not no_split) else []
    if toc and not no_split:
        # 按书签切章，章内按页挂图
        ranges: List[Tuple[str, int, int]] = []
        for i, (t, p0) in enumerate(toc):
            p1 = toc[i + 1][1] if i + 1 < len(toc) else n
            if p1 < p0:
                p1 = p0
            ranges.append((t, p0, max(p0 + 1, p1)))
        # 前言页
        if ranges and ranges[0][1] > 0:
            items = [it for pi in range(0, ranges[0][1]) if (it := page_item(pi))]
            if items:
                chapters.append(ChapterDraft(title="前言", items=items))
        for title_s, p0, p1 in ranges:
            items = [it for pi in range(p0, min(p1, n)) if (it := page_item(pi))]
            chapters.append(ChapterDraft(title=title_s or f"第{p0+1}页", items=items or []))
        toc_used = True
    elif no_split:
        items = [it for pi in range(n) if (it := page_item(pi))]
        chapters = [ChapterDraft(title="全文", items=items)]
        toc_used = False
    else:
        # 无 TOC：每页一章（便于翻页定位），不置原生目录标记
        for pi in range(n):
            it = page_item(pi)
            chapters.append(
                ChapterDraft(title=f"第{pi + 1}页", items=[it] if it else [])
            )
        toc_used = False

    doc.close()
    _prog(progress, 74, "扫描页处理完成")
    return ExtractedBook(
        title=title,
        author=author,
        chapters=chapters or [ChapterDraft(title="全文", items=[])],
        cover=cover,
        source_format="pdf",
        has_toc=toc_used,
    )


def extract_pdf(
    path: Path,
    *,
    chapter_regex: Optional[Sequence[str]] = None,
    use_font_heuristic: bool = True,
    use_pdf_toc: bool = True,
    pdf_toc_max_level: Optional[int] = None,
    force_heuristic: bool = False,
    image_mode: str = DEFAULT_IMAGE_MODE,
    max_width: int = DEFAULT_IMAGE_MAX_W,
    max_height: int = DEFAULT_IMAGE_MAX_H,
    chunk_max: int = 4096,
    include_images: bool = True,
    no_split: bool = False,
    include_cover: bool = True,
    cover_max_width: int = DEFAULT_COVER_MAX_W,
    cover_max_height: int = DEFAULT_COVER_MAX_H,
    cover_max_payload: int = COVER_MAX_PAYLOAD,
    pages_as_images: bool = False,
    page_scale: float = DEFAULT_PDF_PAGE_SCALE,
    page_max_width: int = DEFAULT_PDF_PAGE_MAX_W,
    page_max_height: int = DEFAULT_PDF_PAGE_MAX_H,
    binarize: Optional[BinarizeOptions] = None,
    progress: Optional[ProgressCb] = None,
) -> ExtractedBook:
    try:
        import fitz  # PyMuPDF
    except ImportError as e:
        raise RuntimeError("需要 PyMuPDF：pip install pymupdf") from e

    doc = fitz.open(str(path))
    title = (doc.metadata or {}).get("title") or path.stem
    author = (doc.metadata or {}).get("author") or ""

    if pages_as_images:
        return _extract_pdf_as_page_images(
            doc,
            title=title,
            author=author,
            use_pdf_toc=use_pdf_toc,
            pdf_toc_max_level=pdf_toc_max_level,
            no_split=no_split,
            image_mode=image_mode,
            chunk_max=chunk_max,
            include_cover=include_cover,
            cover_max_width=cover_max_width,
            cover_max_height=cover_max_height,
            cover_max_payload=cover_max_payload,
            page_scale=page_scale,
            page_max_width=page_max_width,
            page_max_height=page_max_height,
            binarize=binarize,
            progress=progress,
        )

    cover: Optional[ContentItem] = None
    if include_cover:
        _prog(progress, 6, "提取封面…")
        raw_cover = _extract_pdf_cover_bytes(doc)
        if raw_cover:
            prep = prepare_cover(
                raw_cover,
                mode=image_mode,
                max_width=cover_max_width,
                max_height=cover_max_height,
                max_payload=cover_max_payload,
                binarize=binarize,
            )
            if prep:
                cover = ContentItem(
                    kind="image",
                    image_fmt=prep.fmt,
                    width=prep.width,
                    height=prep.height,
                    stride=prep.stride,
                    image_data=prep.data,
                )

    page_lines, page_images, span_sizes, all_lines = _page_text_and_images(
        doc, include_images=include_images, progress=progress
    )

    # page_ranges: 与 chapters 对齐，用于把插图按页挂到对应章
    page_ranges: List[Tuple[int, int]] = []
    chapters_spec: List[Tuple[str, List[str]]] = []
    toc_used = False

    if no_split:
        chapters_spec = [("全文", all_lines)]
        page_ranges = [(0, len(page_lines))]
        toc_used = False
    else:
        toc = _toc_entries(doc, max_level=pdf_toc_max_level) if use_pdf_toc else []
        if len(toc) >= 1:
            built = _chapters_from_pdf_toc(page_lines, toc)
            if built:
                toc_used = True
                for title_s, body, pr in built:
                    chapters_spec.append((title_s, body))
                    page_ranges.append(pr)

        if not chapters_spec and force_heuristic:
            # 仅显式要求时才启发式；默认无原生书签 → 单章全文
            chapters_spec = split_lines_to_chapters(all_lines, custom_regex=chapter_regex)
            page_ranges = []
            if (
                use_font_heuristic
                and len(chapters_spec) <= 1
                and len(span_sizes) > 20
            ):
                hits = pdf_font_size_chapter_breaks(span_sizes)
                if len(hits) >= 2:
                    texts = [t for _, t in span_sizes]
                    chapters_spec = []
                    for i, start in enumerate(hits):
                        end = hits[i + 1] if i + 1 < len(hits) else len(texts)
                        title_s = texts[start][:60]
                        body_lines = texts[start + 1 : end]
                        chapters_spec.append((title_s, body_lines))

        if not chapters_spec:
            chapters_spec = [("全文", all_lines)]
            page_ranges = [(0, len(page_lines))]
            toc_used = False

    chapters: List[ChapterDraft] = []
    for ch_title, body in chapters_spec:
        paras = lines_to_paragraphs(body)
        items: List[ContentItem] = []
        if paras:
            items.append(ContentItem(kind="text", paragraphs=paras))
        chapters.append(ChapterDraft(title=ch_title, items=items))

    if include_images and chapters:
        if toc_used and page_ranges and len(page_ranges) == len(chapters):
            # 按章对应页范围挂图（跳过首页已作封面的第一张嵌入图）
            cover_skipped = False
            for ci, (p0, p1) in enumerate(page_ranges):
                for pi in range(max(0, p0), min(p1, len(page_images))):
                    for data in page_images[pi]:
                        if cover is not None and not cover_skipped and pi == 0:
                            cover_skipped = True
                            continue
                        prep = prepare_inline(
                            data,
                            mode=image_mode,
                            max_width=max_width,
                            max_height=max_height,
                            chunk_max=chunk_max,
                            binarize=binarize,
                        )
                        if prep:
                            chapters[ci].items.append(
                                ContentItem(
                                    kind="image",
                                    image_fmt=prep.fmt,
                                    width=prep.width,
                                    height=prep.height,
                                    stride=prep.stride,
                                    image_data=prep.data,
                                )
                            )
        else:
            flat_imgs = [img for imgs in page_images for img in imgs]
            if flat_imgs:
                start = 1 if cover is not None else 0
                n_ch = len(chapters)
                usable = flat_imgs[start:]
                for i, data in enumerate(usable):
                    prep = prepare_inline(
                        data,
                        mode=image_mode,
                        max_width=max_width,
                        max_height=max_height,
                        chunk_max=chunk_max,
                        binarize=binarize,
                    )
                    if not prep:
                        continue
                    ch_i = min(i * n_ch // max(len(usable), 1), n_ch - 1)
                    chapters[ch_i].items.append(
                        ContentItem(
                            kind="image",
                            image_fmt=prep.fmt,
                            width=prep.width,
                            height=prep.height,
                            stride=prep.stride,
                            image_data=prep.data,
                        )
                    )

    doc.close()
    _prog(progress, 74, "PDF 提取完成")
    return ExtractedBook(
        title=title,
        author=author,
        chapters=chapters or [ChapterDraft(title="全文", items=[])],
        cover=cover,
        source_format="pdf",
        has_toc=toc_used,
    )
