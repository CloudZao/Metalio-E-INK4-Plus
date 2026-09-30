"""Metadata.extra 中的 book_id 约定（不改 .ebook 二进制布局）。

规范键名：extra JSON 的 \"book_id\"（字符串）。
- 转换侧：未提供则自动生成 32 位小写 hex（uuid4 去横线）
- 校验：去空白；若含横线的 UUID 则去横线；长度 1–64；仅 [0-9a-zA-Z_-]
- 旧书无该字段：读取返回空串，不视为损坏
"""

from __future__ import annotations

import json
import re
import uuid
from typing import Any, Optional

BOOK_ID_KEY = "book_id"
BOOK_ID_MAX_LEN = 64
_BOOK_ID_RE = re.compile(r"^[0-9a-zA-Z_-]+$")
_UUID_DASHED_RE = re.compile(
    r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"
)


def generate_book_id() -> str:
    """uuid4 去横线，32 位小写 hex。"""
    return uuid.uuid4().hex


def normalize_book_id(raw: Optional[str], *, required: bool = False) -> str:
    """规范化用户/表单输入；非法则抛 ValueError。空串在 required=False 时返回 \"\"。"""
    if raw is None:
        s = ""
    else:
        s = str(raw).strip()
    if not s:
        if required:
            raise ValueError("book_id 不能为空")
        return ""
    if _UUID_DASHED_RE.match(s):
        s = s.replace("-", "").lower()
    if len(s) > BOOK_ID_MAX_LEN:
        raise ValueError(f"book_id 过长（>{BOOK_ID_MAX_LEN}）")
    if not _BOOK_ID_RE.match(s):
        raise ValueError("book_id 仅允许字母、数字、下划线、短横线")
    return s


def resolve_book_id(raw: Optional[str]) -> str:
    """转换写出用：有有效输入则规范化，否则生成新 id。"""
    s = normalize_book_id(raw, required=False)
    return s if s else generate_book_id()


def parse_extra_json(extra: Optional[str]) -> dict[str, Any]:
    """解析 Metadata.extra；非 JSON / 非 object → 空 dict（不抛）。"""
    if not extra or not str(extra).strip():
        return {}
    try:
        obj = json.loads(extra)
    except (json.JSONDecodeError, TypeError):
        return {}
    return obj if isinstance(obj, dict) else {}


def book_id_from_extra(extra: Optional[str]) -> str:
    """从 extra 取 book_id；缺失/非法 → \"\"。"""
    obj = parse_extra_json(extra)
    val = obj.get(BOOK_ID_KEY)
    if val is None:
        return ""
    try:
        return normalize_book_id(str(val), required=False)
    except ValueError:
        return ""


def build_meta_extra(
    *,
    book_id: str,
    source: str = "",
    file_name: str = "",
    has_toc: bool = False,
    pages_as_images: bool = False,
    binarize: Optional[str] = None,
    keep_color: bool = False,
    image_mode: str = "a2i1",
    more: Optional[dict[str, Any]] = None,
) -> str:
    """组装写入 Metadata.extra 的 JSON（保证含规范化 book_id）。"""
    bid = resolve_book_id(book_id)
    payload: dict[str, Any] = {
        BOOK_ID_KEY: bid,
        "source": source,
        "file": file_name,
        "has_toc": bool(has_toc),
        "pages_as_images": bool(pages_as_images),
        "binarize": binarize,
        "keep_color": bool(keep_color),
        "image_mode": image_mode,
    }
    if more:
        for k, v in more.items():
            if k == BOOK_ID_KEY:
                continue
            payload[k] = v
    return json.dumps(payload, ensure_ascii=False)
