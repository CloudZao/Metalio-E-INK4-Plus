"""转换前预览：PDF 页 / EPUB 插图 + 二值化对比（不写 .ebook）。"""

from __future__ import annotations

import base64
import io
import re
from pathlib import Path
from typing import Any, Optional

from .image_prep import BinarizeOptions, fit_size, preview_binarize_pair

_IMG_HREF_RE = re.compile(
    r"""<(?:img|image)\b[^>]+(?:(?:xlink:)?href|src)\s*=\s*['"]([^'"]+)['"]""",
    re.I,
)


def pdf_page_count(path: Path) -> int:
    import fitz

    doc = fitz.open(str(path))
    try:
        return int(doc.page_count)
    finally:
        doc.close()


def render_pdf_page_png(path: Path, page_index: int, scale: float = 1.5) -> bytes:
    """0-based 页码 → PNG bytes。"""
    import fitz

    doc = fitz.open(str(path))
    try:
        n = doc.page_count
        if page_index < 0 or page_index >= n:
            raise ValueError(f"页码越界: {page_index + 1}/{n}")
        scale = max(0.25, min(float(scale), 4.0))
        pix = doc[page_index].get_pixmap(matrix=fitz.Matrix(scale, scale), alpha=False)
        return pix.tobytes("png")
    finally:
        doc.close()


def _all_img_hrefs(html: str) -> list[str]:
    return [m.group(1).strip() for m in _IMG_HREF_RE.finditer(html) if m.group(1).strip()]


def list_epub_images(path: Path) -> list[tuple[str, bytes]]:
    """按阅读顺序收集 EPUB 图片：(逻辑名, 原始字节)。

    顺序：spine 文档内出现的图 → 其余 ITEM_IMAGE / ITEM_COVER。
    """
    import ebooklib
    from ebooklib import epub

    from .extractors.epub import _find_doc, _resolve_href

    book = epub.read_epub(str(path))
    by_name = {it.get_name(): it for it in book.get_items()}
    by_id = {it.get_id(): it for it in book.get_items()}
    seen: set[str] = set()
    out: list[tuple[str, bytes]] = []

    def _add(it) -> None:
        if it is None:
            return
        data = it.get_content() or b""
        if len(data) < 24:
            return
        key = (it.get_name() or it.get_id() or "").strip() or f"img-{len(out)}"
        if key in seen:
            return
        # 跳过明显非位图（svg 文本等）
        head = data[:16]
        if head.lstrip().startswith(b"<") or head.startswith(b"<?xml"):
            return
        seen.add(key)
        out.append((key, bytes(data)))

    # spine 文档中的插图顺序
    for sid, *_rest in book.spine or []:
        it = by_id.get(sid)
        if it is None or it.get_type() != ebooklib.ITEM_DOCUMENT:
            continue
        html = (it.get_content() or b"").decode("utf-8", errors="replace")
        doc_name = it.get_name() or ""
        for href in _all_img_hrefs(html):
            resolved = _resolve_href(doc_name, href)
            _add(_find_doc(by_name, resolved))

    # 封面类型与剩余图片
    for it in book.get_items():
        try:
            t = it.get_type()
        except Exception:
            continue
        if t in (ebooklib.ITEM_IMAGE, ebooklib.ITEM_COVER):
            _add(it)

    return out


def epub_image_count(path: Path) -> int:
    return len(list_epub_images(path))


def _pil_to_png_b64(im) -> str:
    buf = io.BytesIO()
    im.save(buf, format="PNG", optimize=True)
    return base64.b64encode(buf.getvalue()).decode("ascii")


def _preview_from_bytes(
    raw: bytes,
    *,
    index: int,
    total: int,
    max_width: int,
    max_height: int,
    binarize: Optional[BinarizeOptions],
    label: str = "",
) -> dict[str, Any]:
    from PIL import Image

    gray, binary = preview_binarize_pair(
        raw,
        max_width=max_width,
        max_height=max_height,
        binarize=binarize,
    )
    color = Image.open(io.BytesIO(raw)).convert("RGB")
    cw, ch = color.size
    nw, nh = fit_size(cw, ch, max_width, max_height)
    if (nw, nh) != (cw, ch):
        color = color.resize((nw, nh), Image.Resampling.LANCZOS)

    opts = binarize or BinarizeOptions()
    return {
        "ok": True,
        "page": index,
        "pages": total,
        "label": label,
        "width": binary.size[0],
        "height": binary.size[1],
        "method": opts.method,
        "threshold": opts.threshold,
        "contrast": opts.contrast,
        "window": opts.window,
        "k": opts.k,
        "color_png": _pil_to_png_b64(color),
        "gray_png": _pil_to_png_b64(gray),
        "binary_png": _pil_to_png_b64(binary),
    }


def preview_pdf_binarize(
    path: Path,
    *,
    page_index: int = 0,
    scale: float = 1.5,
    max_width: int = 360,
    max_height: int = 600,
    binarize: Optional[BinarizeOptions] = None,
) -> dict[str, Any]:
    """PDF 指定页预览。"""
    png = render_pdf_page_png(path, page_index, scale=scale)
    pages = pdf_page_count(path)
    return _preview_from_bytes(
        png,
        index=page_index,
        total=pages,
        max_width=max_width,
        max_height=max_height,
        binarize=binarize,
        label=f"page-{page_index + 1}",
    )


def preview_epub_binarize(
    path: Path,
    *,
    image_index: int = 0,
    max_width: int = 360,
    max_height: int = 600,
    binarize: Optional[BinarizeOptions] = None,
) -> dict[str, Any]:
    """EPUB 指定序号插图预览。"""
    images = list_epub_images(path)
    if not images:
        raise ValueError("该 EPUB 没有可预览的位图")
    if image_index < 0 or image_index >= len(images):
        raise ValueError(f"图片序号越界: {image_index + 1}/{len(images)}")
    name, raw = images[image_index]
    return _preview_from_bytes(
        raw,
        index=image_index,
        total=len(images),
        max_width=max_width,
        max_height=max_height,
        binarize=binarize,
        label=name,
    )


def preview_source_binarize(
    path: Path,
    *,
    fmt: str,
    index: int = 0,
    scale: float = 1.5,
    max_width: int = 360,
    max_height: int = 600,
    binarize: Optional[BinarizeOptions] = None,
) -> dict[str, Any]:
    """统一入口：pdf / epub。"""
    fmt = (fmt or path.suffix.lstrip(".")).lower()
    if fmt == "pdf":
        return preview_pdf_binarize(
            path,
            page_index=index,
            scale=scale,
            max_width=max_width,
            max_height=max_height,
            binarize=binarize,
        )
    if fmt == "epub":
        return preview_epub_binarize(
            path,
            image_index=index,
            max_width=max_width,
            max_height=max_height,
            binarize=binarize,
        )
    raise ValueError(f"不支持预览的格式: {fmt}")
