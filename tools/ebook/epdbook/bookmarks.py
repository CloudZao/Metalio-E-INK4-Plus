"""调试用书签：旁路 JSON，不写入 .ebook 本体。"""

from __future__ import annotations

import json
import time
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import List, Optional


@dataclass
class Bookmark:
    id: str
    chapter: int
    piece: int  # 章内 ContentPiece 下标
    note: str = ""
    preview: str = ""
    created: float = 0.0

    @staticmethod
    def create(chapter: int, piece: int, preview: str = "", note: str = "") -> "Bookmark":
        return Bookmark(
            id=uuid.uuid4().hex[:12],
            chapter=chapter,
            piece=max(0, piece),
            note=note,
            preview=(preview or "")[:80],
            created=time.time(),
        )


@dataclass
class BookmarkStore:
    ebook_path: str
    bookmarks: List[Bookmark] = field(default_factory=list)

    @property
    def sidecar_path(self) -> Path:
        return Path(self.ebook_path).with_suffix(Path(self.ebook_path).suffix + ".bookmarks.json")


def load_bookmarks(ebook_path: Path) -> BookmarkStore:
    store = BookmarkStore(ebook_path=str(ebook_path.resolve()))
    path = store.sidecar_path
    if not path.is_file():
        return store
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        for raw in data.get("bookmarks", []):
            store.bookmarks.append(
                Bookmark(
                    id=str(raw.get("id") or uuid.uuid4().hex[:12]),
                    chapter=int(raw.get("chapter", 0)),
                    piece=int(raw.get("piece", 0)),
                    note=str(raw.get("note") or ""),
                    preview=str(raw.get("preview") or ""),
                    created=float(raw.get("created") or 0),
                )
            )
    except (OSError, json.JSONDecodeError, TypeError, ValueError):
        pass
    return store


def save_bookmarks(store: BookmarkStore) -> Path:
    path = store.sidecar_path
    payload = {
        "ebook_path": store.ebook_path,
        "bookmarks": [asdict(b) for b in store.bookmarks],
    }
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    return path


def add_bookmark(store: BookmarkStore, bm: Bookmark) -> None:
    store.bookmarks.append(bm)


def remove_bookmark(store: BookmarkStore, bm_id: str) -> bool:
    before = len(store.bookmarks)
    store.bookmarks = [b for b in store.bookmarks if b.id != bm_id]
    return len(store.bookmarks) < before
