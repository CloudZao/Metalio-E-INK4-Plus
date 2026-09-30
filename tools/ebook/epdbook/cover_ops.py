"""封面编码与 .ebook 尾部补丁（与正文 Data 分轨）。

约定（与 SPEC / EbookWriter 对齐）：
- 规范封面起点：data_offset + data_size
- 仅改 Header 的 cover_* / HAS_COVER / crc32；不改 IMG_A2I1、不碰 Data/Index
- 用户主动选图时编码失败必须抛错，禁止静默无封面
"""

from __future__ import annotations

import os
import struct
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Union

from .format import (
    COVER_MAX_PAYLOAD,
    DEFAULT_COVER_MAX_H,
    DEFAULT_COVER_MAX_W,
    FLAG_HAS_COVER,
    HEADER_SIZE,
    HEADER_STRUCT,
    IMG_A2I1,
    IMG_GRAY,
    IMG_JPEG,
    IMG_PNG,
    crc32_header,
    pack_image_payload,
    parse_size_box,
)
from .image_prep import (
    DEFAULT_BINARIZE_METHOD,
    BinarizeOptions,
    PreparedImage,
    binarize_options_from_args,
    prepare_cover,
)
from .reader import load_cover_payload, open_ebook
from .writer import ContentItem

PathLike = Union[str, Path]
ImageInput = Union[bytes, PathLike]

_FMT_NAME = {
    IMG_JPEG: "jpeg",
    IMG_PNG: "png",
    IMG_A2I1: "a2i1",
    IMG_GRAY: "gray",
}


@dataclass
class CoverInfo:
    has_cover: bool
    offset: int = 0
    size: int = 0
    fmt: int = 0
    width: int = 0
    height: int = 0
    stride: int = 0
    crc_ok: bool = False
    data_end: int = 0

    @property
    def fmt_name(self) -> str:
        return _FMT_NAME.get(self.fmt, f"fmt{self.fmt}")


@dataclass
class CoverMutateResult:
    path: Path
    has_cover: bool
    cover_size: int = 0
    width: int = 0
    height: int = 0
    fmt: int = 0
    action: str = ""  # set | clear


def _read_image_bytes(source: ImageInput) -> bytes:
    if isinstance(source, (bytes, bytearray, memoryview)):
        data = bytes(source)
        if not data:
            raise ValueError("封面图片为空")
        return data
    path = Path(source)
    if not path.is_file():
        raise FileNotFoundError(f"封面图片不存在: {path}")
    data = path.read_bytes()
    if not data:
        raise ValueError(f"封面图片为空: {path}")
    return data


def encode_cover_image(
    source: ImageInput,
    *,
    cover_max: str = f"{DEFAULT_COVER_MAX_W}x{DEFAULT_COVER_MAX_H}",
    max_payload: int = COVER_MAX_PAYLOAD,
    binarize: Optional[BinarizeOptions] = None,
    keep_color: bool = False,
) -> ContentItem:
    """将用户图片编码为封面 ContentItem；失败抛错。"""
    data = _read_image_bytes(source)
    max_w, max_h = parse_size_box(cover_max)
    mode = "jpeg" if keep_color else "a2i1"
    if binarize is None and not keep_color:
        binarize = binarize_options_from_args(method=DEFAULT_BINARIZE_METHOD)
    prep = prepare_cover(
        data,
        mode=mode,
        max_width=max_w,
        max_height=max_h,
        max_payload=max_payload,
        binarize=None if keep_color else binarize,
    )
    if prep is None:
        raise ValueError(
            f"封面编码失败（无法压入 {max_w}x{max_h} / {max_payload}B 预算）"
        )
    return prepared_to_cover_item(prep)


def prepared_to_cover_item(prep: PreparedImage) -> ContentItem:
    return ContentItem(
        kind="image",
        image_fmt=prep.fmt,
        width=prep.width,
        height=prep.height,
        stride=prep.stride,
        image_data=prep.data,
    )


def pack_cover_payload(
    item: ContentItem,
    *,
    max_payload: int = COVER_MAX_PAYLOAD,
) -> bytes:
    if item.kind != "image" or not item.image_data:
        raise ValueError("无效封面 ContentItem")
    payload = pack_image_payload(
        item.image_fmt,
        item.width,
        item.height,
        item.image_data,
        item.stride,
    )
    if len(payload) > max_payload:
        raise ValueError(
            f"cover ImagePayload {len(payload)} > cover_max {max_payload}; rescale first"
        )
    return payload


def _data_end(book) -> int:
    hf = book.header_fields
    return int(hf["data_offset"]) + int(hf["data_size"])


def _validate_layout_for_mutate(book, file_size: int) -> int:
    """返回规范 data_end；布局不合法则抛错。"""
    if not book.crc_ok:
        raise ValueError("Header CRC 校验失败，拒绝修改封面（请先修复或重新转换）")
    data_end = _data_end(book)
    if data_end < HEADER_SIZE or data_end > file_size:
        raise ValueError(
            f"数据区边界异常 data_end={data_end} file_size={file_size}"
        )

    has = book.has_cover
    if has:
        if book.cover_offset != data_end:
            raise ValueError(
                f"封面偏移不规范 cover_offset={book.cover_offset} "
                f"!= data_end={data_end}，拒绝原地补丁"
            )
        if book.cover_size <= 0:
            raise ValueError("HAS_COVER 但 cover_size=0")
        end = book.cover_offset + book.cover_size
        if end > file_size:
            raise ValueError(
                f"封面越界 offset+size={end} > file_size={file_size}"
            )
    elif book.cover_offset != 0 or book.cover_size != 0:
        raise ValueError(
            f"无 HAS_COVER 但 cover_offset/size 非零 "
            f"({book.cover_offset}/{book.cover_size})"
        )
    return data_end


def cover_info(ebook_path: PathLike) -> CoverInfo:
    path = Path(ebook_path)
    book = open_ebook(path)
    data_end = _data_end(book)
    info = CoverInfo(
        has_cover=book.has_cover,
        offset=book.cover_offset,
        size=book.cover_size,
        crc_ok=book.crc_ok,
        data_end=data_end,
    )
    if not book.has_cover:
        return info
    piece = load_cover_payload(book)
    if piece:
        info.fmt = piece.image_fmt
        info.width = piece.width
        info.height = piece.height
        info.stride = piece.stride
    return info


def _rewrite_header_cover(
    header: bytes,
    *,
    flags: int,
    cover_offset: int,
    cover_size: int,
) -> bytes:
    fields = list(HEADER_STRUCT.unpack(header))
    # 0 magic, 1 ver, 2 flags, … 14 cover_off, 15 cover_sz, 16 crc, 17 reserved
    fields[2] = flags
    fields[14] = cover_offset
    fields[15] = cover_size
    fields[16] = 0
    body = HEADER_STRUCT.pack(*fields)
    crc = crc32_header(body[:56])
    return body[:56] + struct.pack("<I", crc) + body[60:]


def _atomic_write_ebook(path: Path, body_without_header: bytes, new_header: bytes) -> None:
    """body_without_header = file[HEADER_SIZE:] 的新内容（已含可选封面）。"""
    if len(new_header) != HEADER_SIZE:
        raise ValueError("header size mismatch")
    parent = path.parent
    parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(prefix=f".{path.stem}.", suffix=".ebook.tmp", dir=str(parent))
    tmp_path = Path(tmp_name)
    try:
        with os.fdopen(fd, "wb") as fp:
            fp.write(new_header)
            fp.write(body_without_header)
            fp.flush()
            os.fsync(fp.fileno())
        os.replace(tmp_path, path)
    except Exception:
        try:
            if tmp_path.exists():
                tmp_path.unlink()
        except OSError:
            pass
        raise


def _apply_cover_payload(ebook_path: Path, payload: Optional[bytes]) -> CoverMutateResult:
    path = Path(ebook_path)
    if not path.is_file():
        raise FileNotFoundError(str(path))

    book = open_ebook(path)
    raw = path.read_bytes()
    file_size = len(raw)
    data_end = _validate_layout_for_mutate(book, file_size)

    if len(raw) < data_end:
        raise ValueError("文件短于数据区")

    prefix = raw[:data_end]  # includes header
    old_header = prefix[:HEADER_SIZE]
    rest = prefix[HEADER_SIZE:]

    flags = book.flags
    if payload:
        cover_offset = data_end
        cover_size = len(payload)
        flags = flags | FLAG_HAS_COVER
        new_rest = rest + payload
        action = "set"
    else:
        cover_offset = 0
        cover_size = 0
        flags = flags & ~FLAG_HAS_COVER
        new_rest = rest
        action = "clear"

    new_header = _rewrite_header_cover(
        old_header,
        flags=flags,
        cover_offset=cover_offset,
        cover_size=cover_size,
    )
    _atomic_write_ebook(path, new_rest, new_header)

    # 自检
    verify = open_ebook(path)
    if not verify.crc_ok:
        raise RuntimeError("写后 Header CRC 自检失败")
    if payload:
        if not verify.has_cover or verify.cover_size != cover_size:
            raise RuntimeError("写后封面元数据自检失败")
        if load_cover_payload(verify) is None:
            raise RuntimeError("写后封面载荷不可读")
        piece = load_cover_payload(verify)
        assert piece is not None
        return CoverMutateResult(
            path=path,
            has_cover=True,
            cover_size=cover_size,
            width=piece.width,
            height=piece.height,
            fmt=piece.image_fmt,
            action=action,
        )
    if verify.has_cover or verify.cover_offset != 0 or verify.cover_size != 0:
        raise RuntimeError("清除封面后自检失败")
    return CoverMutateResult(
        path=path,
        has_cover=False,
        cover_size=0,
        action=action,
    )


def set_ebook_cover(
    ebook_path: PathLike,
    source: ImageInput,
    *,
    cover_max: str = f"{DEFAULT_COVER_MAX_W}x{DEFAULT_COVER_MAX_H}",
    max_payload: int = COVER_MAX_PAYLOAD,
    binarize: Optional[BinarizeOptions] = None,
    keep_color: bool = False,
) -> CoverMutateResult:
    """编码用户图并原地设置/替换 .ebook 封面。"""
    item = encode_cover_image(
        source,
        cover_max=cover_max,
        max_payload=max_payload,
        binarize=binarize,
        keep_color=keep_color,
    )
    payload = pack_cover_payload(item, max_payload=max_payload)
    return _apply_cover_payload(Path(ebook_path), payload)


def clear_ebook_cover(ebook_path: PathLike) -> CoverMutateResult:
    """清除独立封面区（截断到 data_end）。"""
    return _apply_cover_payload(Path(ebook_path), None)
