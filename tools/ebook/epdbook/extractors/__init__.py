"""提取器公共类型。"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Protocol, Sequence

from ..writer import ChapterDraft, ContentItem


@dataclass
class ExtractedBook:
    title: str = ""
    author: str = ""
    lang: str = "zh"
    chapters: List[ChapterDraft] = field(default_factory=list)
    cover: Optional[ContentItem] = None
    source_format: str = ""
    # True=来自 PDF Outline / EPUB nav·NCX 等原生目录；False=无目录（单章全文）
    has_toc: bool = False


class Extractor(Protocol):
    def extract(self, path: Path, **kwargs) -> ExtractedBook: ...


def text_item(*paragraphs: str) -> ContentItem:
    return ContentItem(kind="text", paragraphs=[p for p in paragraphs if p])


def paragraphs_from_plain(text: str) -> List[str]:
    from ..chapter_detect import lines_to_paragraphs

    return lines_to_paragraphs(text.replace("\r\n", "\n").replace("\r", "\n").split("\n"))
