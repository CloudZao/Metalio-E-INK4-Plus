"""纯文本提取 + 启发式/自定义章节切分。

切章规则见 `epdbook.chapter_detect`（对齐固件 txt_chapter，并扩展网文常见标题）。
识别到 ≥2 章时置 has_toc，写入 .ebook 目录供 Web/ESP32 使用。
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import List, Optional, Sequence, Tuple

from ..chapter_detect import lines_to_paragraphs, split_lines_to_chapters
from ..writer import ChapterDraft, ContentItem, chapter_draft_has_content
from . import ExtractedBook


def _decode_txt(data: bytes) -> str:
    for enc in ("utf-8-sig", "utf-8", "gb18030", "gbk", "big5", "latin-1"):
        try:
            return data.decode(enc)
        except UnicodeDecodeError:
            continue
    return data.decode("utf-8", errors="replace")


def _guess_meta(lines: Sequence[str]) -> Tuple[Optional[str], Optional[str]]:
    """从文首若干行猜测书名/作者（不强制）。"""
    title: Optional[str] = None
    author: Optional[str] = None
    author_re = re.compile(
        r"^(?:作者|作\s*者|著者|编剧|原著)\s*[:：\s　]+(.+)$"
    )
    title_re = re.compile(r"^(?:书名|作品名|题目)\s*[:：\s　]+(.+)$")
    for line in lines[:60]:
        s = (line or "").strip()
        if not s or len(s) > 80:
            continue
        m = author_re.match(s)
        if m and not author:
            author = m.group(1).strip()
            continue
        m = title_re.match(s)
        if m and not title:
            title = m.group(1).strip()
            continue
    return title, author


def extract_txt(
    path: Path,
    *,
    chapter_regex: Optional[Sequence[str]] = None,
    no_split: bool = False,
) -> ExtractedBook:
    text = _decode_txt(path.read_bytes())
    # 统一换行
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    lines = text.split("\n")
    meta_title, meta_author = _guess_meta(lines)
    title = meta_title or path.stem

    if no_split:
        paras = lines_to_paragraphs(lines)
        chapters = [ChapterDraft(title="全文", items=[ContentItem(kind="text", paragraphs=paras)])]
    else:
        slices = split_lines_to_chapters(lines, custom_regex=chapter_regex)
        chapters = []
        for ch_title, body in slices:
            paras = lines_to_paragraphs(body)
            chapters.append(
                ChapterDraft(
                    title=ch_title,
                    items=[ContentItem(kind="text", paragraphs=paras)] if paras else [],
                )
            )
        # 去掉切章后仍无正文的空章（目录假命中等）
        chapters = [c for c in chapters if chapter_draft_has_content(c)]
        if not chapters:
            paras = lines_to_paragraphs(lines)
            chapters = [
                ChapterDraft(title="全文", items=[ContentItem(kind="text", paragraphs=paras)])
            ]

    has_toc = len(chapters) > 1 and not no_split
    return ExtractedBook(
        title=title,
        author=meta_author or "",
        chapters=chapters,
        source_format="txt",
        has_toc=has_toc,
    )
