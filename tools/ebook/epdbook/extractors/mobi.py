"""MOBI/AZW 提取：优先用 mobi 库解包为 HTML，再走与 EPUB 类似的流解析。"""

from __future__ import annotations

import shutil
from pathlib import Path
from typing import List, Optional, Sequence

from ..format import DEFAULT_IMAGE_MAX_H, DEFAULT_IMAGE_MAX_W, DEFAULT_IMAGE_MODE
from ..writer import ChapterDraft, ContentItem
from . import ExtractedBook
from .epub import _HtmlToFlow, _image_to_item


def extract_mobi(
    path: Path,
    *,
    image_mode: str = DEFAULT_IMAGE_MODE,
    max_width: int = DEFAULT_IMAGE_MAX_W,
    max_height: int = DEFAULT_IMAGE_MAX_H,
    chunk_max: int = 4096,
    include_images: bool = True,
    chapter_regex: Optional[Sequence[str]] = None,
    include_cover: bool = True,
    binarize=None,
    **_cover_kwargs,
) -> ExtractedBook:
    """
    依赖 `mobi`（pip install mobi）：将 mobi 解到临时目录，读取 spine HTML。
    若失败，尝试用 ebooklib 不可行时给出明确错误。
    """
    try:
        import mobi
    except ImportError as e:
        raise RuntimeError(
            "需要 mobi：pip install mobi（或先将文件转为 epub 再转换）"
        ) from e

    # mobi.extract 返回 (tempdir, filepath)
    tempdir, filepath = mobi.extract(str(path))
    try:
        root = Path(tempdir)
        # 解包后常见结构：*.html / images/
        html_files = sorted(root.rglob("*.html")) + sorted(root.rglob("*.htm"))
        if not html_files:
            # 有些产出 epub
            epubs = list(root.rglob("*.epub"))
            if epubs:
                from .epub import extract_epub

                return extract_epub(
                    epubs[0],
                    image_mode=image_mode,
                    max_width=max_width,
                    max_height=max_height,
                    chunk_max=chunk_max,
                    include_images=include_images,
                    chapter_regex=chapter_regex,
                    binarize=binarize,
                )
            raise RuntimeError("mobi 解包后未找到 HTML/EPUB")

        chapters: List[ChapterDraft] = []
        image_dirs = [p for p in root.rglob("*") if p.is_dir() and "image" in p.name.lower()]
        image_map = {}
        for d in [root] + image_dirs:
            for ext in ("*.jpg", "*.jpeg", "*.png", "*.gif", "*.webp"):
                for img in d.glob(ext):
                    image_map[img.name.lower()] = img

        for hf in html_files:
            html = hf.read_text(encoding="utf-8", errors="replace")
            parser = _HtmlToFlow()
            parser.feed(html)
            parser.close()
            title = _first_text(parser.events) or hf.stem
            items: List[ContentItem] = []
            paras: List[str] = []

            def flush() -> None:
                nonlocal paras
                if paras:
                    items.append(ContentItem(kind="text", paragraphs=list(paras)))
                    paras = []

            for kind, val in parser.events:
                if kind == "text":
                    paras.append(str(val))
                elif kind == "img" and include_images:
                    flush()
                    name = Path(str(val)).name.lower()
                    img_path = image_map.get(name)
                    if img_path and img_path.is_file():
                        try:
                            items.append(
                                _image_to_item(
                                    img_path.read_bytes(),
                                    image_mode=image_mode,
                                    max_width=max_width,
                                    max_height=max_height,
                                    chunk_max=chunk_max,
                                    binarize=binarize,
                                )
                            )
                        except Exception:
                            pass
            flush()
            chapters.append(ChapterDraft(title=title[:60], items=items))

        # 若只有单文件长文，用正则再切
        if len(chapters) == 1 and chapter_regex:
            from ..chapter_detect import lines_to_paragraphs, split_lines_to_chapters

            text = "\n".join(
                p
                for it in chapters[0].items
                if it.kind == "text"
                for p in it.paragraphs
            )
            slices = split_lines_to_chapters(text.split("\n"), custom_regex=chapter_regex)
            if len(slices) > 1:
                chapters = [
                    ChapterDraft(
                        title=t,
                        items=[ContentItem(kind="text", paragraphs=lines_to_paragraphs(b))],
                    )
                    for t, b in slices
                ]

        return ExtractedBook(
            title=path.stem,
            author="",
            chapters=chapters or [ChapterDraft(title="全文", items=[])],
            cover=None,
            source_format="mobi",
            has_toc=len(chapters) > 1,
        )
    finally:
        try:
            shutil.rmtree(tempdir, ignore_errors=True)
        except Exception:
            pass


def _first_text(events) -> str:
    for kind, val in events[:10]:
        if kind == "text":
            t = str(val).strip()
            if 2 <= len(t) <= 60:
                return t
    return ""
