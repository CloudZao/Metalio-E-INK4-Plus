"""共享转换入口：CLI 与 Web 共用。"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Optional

from .format import (
    COVER_MAX_PAYLOAD,
    DEFAULT_CHAPTER_MAX_BYTES,
    DEFAULT_CHUNK_MAX,
    DEFAULT_COVER_MAX_H,
    DEFAULT_COVER_MAX_W,
    DEFAULT_FONT_PX,
    DEFAULT_IMAGE_MAX_H,
    DEFAULT_IMAGE_MAX_W,
    DEFAULT_PDF_PAGE_MAX_H,
    DEFAULT_PDF_PAGE_MAX_W,
    DEFAULT_PDF_PAGE_SCALE,
    DEVICE_CHAPTER_MAX_UNCOMP,
    DEVICE_CHUNK_MAX,
    FLAG_HAS_COVER,
    FLAG_HAS_TOC,
    FLAG_IMG_A2I1,
    FLAG_PAGE_IMAGES,
    parse_size_box,
)
from .image_prep import (
    DEFAULT_BINARIZE_METHOD,
    BinarizeOptions,
    binarize_options_from_args,
)
from .writer import EbookWriter

_IMAGE_MODE = "a2i1"

# percent 0–100，message 给人看的阶段说明
ProgressCb = Callable[[int, str], None]


def _report(progress: Optional[ProgressCb], pct: int, msg: str) -> None:
    if progress is None:
        return
    pct = max(0, min(100, int(pct)))
    try:
        progress(pct, msg)
    except Exception:
        pass


@dataclass
class ConvertOptions:
    title: str = ""
    author: str = ""
    lang: str = "zh"
    # Metadata.extra.book_id；空则转换时自动生成 uuid hex
    book_id: str = ""
    font_px: int = DEFAULT_FONT_PX
    chunk_max: int = DEFAULT_CHUNK_MAX
    chapter_regex: Optional[list[str]] = None
    no_split: bool = False
    # 超大章按文本体积拆分（默认开）；与设备章加载上限对齐
    chapter_max_bytes: int = DEFAULT_CHAPTER_MAX_BYTES
    no_chapter_size_split: bool = False
    no_images: bool = False
    no_cover: bool = False
    # 用户指定封面图（Path）；优先级高于源文件提取封面；与 no_cover 互斥
    cover_image: Optional[Path] = None
    image_max: str = f"{DEFAULT_IMAGE_MAX_W}x{DEFAULT_IMAGE_MAX_H}"
    cover_max: str = f"{DEFAULT_COVER_MAX_W}x{DEFAULT_COVER_MAX_H}"
    cover_max_payload: int = COVER_MAX_PAYLOAD
    no_font_heuristic: bool = False
    ignore_pdf_toc: bool = False
    pdf_toc_max_level: Optional[int] = None
    force_heuristic: bool = False
    # 扫描 PDF：整页转图
    pdf_pages_as_images: bool = False
    pdf_page_scale: float = DEFAULT_PDF_PAGE_SCALE
    pdf_page_max: str = f"{DEFAULT_PDF_PAGE_MAX_W}x{DEFAULT_PDF_PAGE_MAX_H}"
    force_format: Optional[str] = None
    # A2I1 二值化（转换侧；格式不变）
    binarize_method: str = DEFAULT_BINARIZE_METHOD
    binarize_threshold: int = 128
    binarize_contrast: float = 1.0
    binarize_window: int = 25
    binarize_k: float = 0.34
    # 保留彩色 JPEG，不二值化；设备阅读时按原生 EPUB 同款 Bayer 转黑白
    keep_color: bool = False

    def binarize_opts(self) -> BinarizeOptions:
        return binarize_options_from_args(
            method=self.binarize_method,
            threshold=self.binarize_threshold,
            contrast=self.binarize_contrast,
            window=self.binarize_window,
            k=self.binarize_k,
        )


@dataclass
class ConvertResult:
    output: Path
    title: str
    author: str
    chapters: int
    has_toc: bool
    has_cover: bool
    source_format: str
    book_id: str = ""
    warnings: list[str] = field(default_factory=list)
    extra: dict[str, Any] = field(default_factory=dict)


def detect_format(path: Path, force: Optional[str] = None) -> str:
    if force:
        return force.lower().lstrip(".")
    ext = path.suffix.lower().lstrip(".")
    if ext in ("txt", "text"):
        return "txt"
    if ext == "pdf":
        return "pdf"
    if ext == "epub":
        return "epub"
    if ext in ("mobi", "azw", "azw3", "prc"):
        return "mobi"
    raise ValueError(f"无法识别格式: {path.suffix}")


def convert_file(
    src: Path,
    out: Path,
    opts: Optional[ConvertOptions] = None,
    *,
    progress: Optional[ProgressCb] = None,
) -> ConvertResult:
    opts = opts or ConvertOptions()
    if not src.is_file():
        raise FileNotFoundError(str(src))

    _report(progress, 2, "准备转换…")
    img_w, img_h = parse_size_box(opts.image_max)
    cov_w, cov_h = parse_size_box(opts.cover_max)
    page_w, page_h = parse_size_box(opts.pdf_page_max)
    fmt = detect_format(src, opts.force_format)

    chunk_max = opts.chunk_max
    warnings: list[str] = []
    # 默认 480×720 A2I1 约 43KB；未显式加大 chunk 时自动抬到设备上限
    need_large_images = opts.pdf_pages_as_images or (not opts.no_images)
    if need_large_images and chunk_max <= DEFAULT_CHUNK_MAX:
        chunk_max = DEVICE_CHUNK_MAX
        warnings.append(f"大图默认框，自动 chunk_max={chunk_max}")
    chunk_max = min(chunk_max, DEVICE_CHUNK_MAX)
    # 整页转图：Web/旧表单仍可能传 432×720，抬到阅读区全宽默认
    if opts.pdf_pages_as_images and (page_w, page_h) == (432, 720):
        page_w, page_h = DEFAULT_PDF_PAGE_MAX_W, DEFAULT_PDF_PAGE_MAX_H
        warnings.append(f"整页转图页框抬至 {page_w}x{page_h}（全宽阅读区）")

    binarize = opts.binarize_opts()
    image_mode = "jpeg" if opts.keep_color else _IMAGE_MODE
    common = dict(
        image_mode=image_mode,
        max_width=img_w,
        max_height=img_h,
        chunk_max=chunk_max,
        include_images=not opts.no_images,
        chapter_regex=opts.chapter_regex,
        include_cover=not opts.no_cover,
        cover_max_width=cov_w,
        cover_max_height=cov_h,
        cover_max_payload=opts.cover_max_payload,
        binarize=binarize,
        progress=progress,
    )

    _report(progress, 5, f"解析 {fmt.upper()}…")
    if fmt == "txt":
        from .extractors.txt import extract_txt

        book = extract_txt(src, chapter_regex=opts.chapter_regex, no_split=opts.no_split)
        _report(progress, 70, "文本提取完成")
    elif fmt == "epub":
        from .extractors.epub import extract_epub

        book = extract_epub(src, **common)
    elif fmt == "pdf":
        from .extractors.pdf import extract_pdf

        book = extract_pdf(
            src,
            use_font_heuristic=not opts.no_font_heuristic,
            use_pdf_toc=not opts.ignore_pdf_toc,
            pdf_toc_max_level=opts.pdf_toc_max_level,
            force_heuristic=opts.force_heuristic,
            no_split=opts.no_split,
            pages_as_images=opts.pdf_pages_as_images,
            page_scale=opts.pdf_page_scale,
            page_max_width=page_w,
            page_max_height=page_h,
            **common,
        )
    elif fmt == "mobi":
        from .extractors.mobi import extract_mobi

        book = extract_mobi(src, **common)
        _report(progress, 70, "MOBI 提取完成")
    else:
        raise ValueError(f"不支持的格式: {fmt}")

    if not book.has_toc:
        if len(book.chapters) <= 1:
            warnings.append("源文件没有原生目录 → 单章「全文」")
        else:
            warnings.append(f"无原生目录标记；当前 {len(book.chapters)} 章（未置 HAS_TOC）")

    from .writer import chapter_draft_has_content

    before_n = len(book.chapters)
    book.chapters = [c for c in book.chapters if chapter_draft_has_content(c)]
    dropped = before_n - len(book.chapters)
    if dropped:
        warnings.append(f"已跳过 {dropped} 个空章节")
    if not book.chapters:
        raise ValueError("全书无有效内容（全部章节为空）")

    if not opts.no_chapter_size_split:
        from .chapter_split import split_oversized_chapters

        max_b = int(opts.chapter_max_bytes or DEFAULT_CHAPTER_MAX_BYTES)
        if max_b < 1024 or max_b >= DEVICE_CHAPTER_MAX_UNCOMP:
            raise ValueError(
                f"chapter_max_bytes 须满足 1024 ≤ n < {DEVICE_CHAPTER_MAX_UNCOMP}（设备硬上限）"
            )
        book.chapters, n_split = split_oversized_chapters(
            book.chapters, max_chapter_bytes=max_b
        )
        if n_split:
            warnings.append(
                f"超大章按体积拆分：{n_split} 章 → 现共 {len(book.chapters)} 章"
                f"（上限 {max_b} 字节解压合计；体积拆章本身不置 HAS_TOC）"
            )
            _report(progress, 76, f"体积拆章后 {len(book.chapters)} 章…")
        # 拆后可能产生空壳（理论上已过滤）；再扫一遍
        book.chapters = [c for c in book.chapters if chapter_draft_has_content(c)]
        if not book.chapters:
            raise ValueError("全书无有效内容（拆章后为空）")

    _report(progress, 78, f"写入 {len(book.chapters)} 章…")
    from .meta_extra import build_meta_extra, book_id_from_extra

    # 非法 book_id 在写出前失败，避免写出无 id / 坏 id 的半成品
    try:
        extra_json = build_meta_extra(
            book_id=opts.book_id,
            source=book.source_format,
            file_name=src.name,
            has_toc=book.has_toc,
            pages_as_images=bool(opts.pdf_pages_as_images and fmt == "pdf"),
            binarize=binarize.method if not opts.keep_color else None,
            keep_color=opts.keep_color,
            image_mode=image_mode,
        )
    except ValueError as e:
        raise ValueError(f"book_id 无效: {e}") from e
    written_book_id = book_id_from_extra(extra_json)

    writer = EbookWriter(
        title=opts.title or book.title,
        author=opts.author or book.author,
        lang=opts.lang or book.lang,
        extra=extra_json,
        default_font_px=opts.font_px,
        chunk_max_uncomp=chunk_max,
        prefer_a2i1=not opts.keep_color,
        has_toc=book.has_toc,
        page_images=bool(opts.pdf_pages_as_images and fmt == "pdf"),
    )
    n_ch = max(1, len(book.chapters))
    for i, ch in enumerate(book.chapters):
        writer.add_chapter(ch)
        if i % max(1, n_ch // 20) == 0 or i + 1 == n_ch:
            _report(progress, 78 + int(12 * (i + 1) / n_ch), f"打包章节 {i + 1}/{n_ch}")

    if opts.no_cover and opts.cover_image:
        raise ValueError("--no-cover 与自定义封面不能同时使用")

    cover_ok = False
    if opts.no_cover:
        writer.clear_cover()
    elif opts.cover_image:
        from .cover_ops import encode_cover_image

        # 用户显式指定：编码失败则整次转换失败（禁止静默无封面）
        _report(progress, 90, "编码自定义封面…")
        cover_item = encode_cover_image(
            opts.cover_image,
            cover_max=opts.cover_max,
            max_payload=opts.cover_max_payload,
            binarize=binarize,
            keep_color=opts.keep_color,
        )
        writer.set_cover_image(
            cover_item.image_fmt,
            cover_item.width,
            cover_item.height,
            cover_item.image_data,
            cover_item.stride,
            max_payload=opts.cover_max_payload,
        )
        cover_ok = True
        if book.cover:
            warnings.append("已用自定义封面覆盖源文件封面")
        else:
            warnings.append("已注入自定义封面")
    elif book.cover and book.cover.kind == "image":
        try:
            writer.set_cover_image(
                book.cover.image_fmt,
                book.cover.width,
                book.cover.height,
                book.cover.image_data,
                book.cover.stride,
                max_payload=opts.cover_max_payload,
            )
            cover_ok = True
        except ValueError:
            writer.clear_cover()
            warnings.append("封面超预算已丢弃")

    _report(progress, 94, "写出文件…")
    out.parent.mkdir(parents=True, exist_ok=True)
    writer.write(out)
    _report(progress, 100, "完成")

    return ConvertResult(
        output=out,
        title=writer.title,
        author=writer.author,
        chapters=len(writer.chapters),
        has_toc=book.has_toc,
        has_cover=cover_ok,
        source_format=book.source_format,
        book_id=written_book_id,
        warnings=warnings,
        extra={
            "chunk_max": chunk_max,
            "flags_hint": "jpeg" if opts.keep_color else "a2i1",
            "binarize": binarize.method if not opts.keep_color else None,
            "keep_color": opts.keep_color,
            "page_images": bool(opts.pdf_pages_as_images and fmt == "pdf"),
            "book_id": written_book_id,
        },
    )


def inspect_ebook(path: Path) -> dict[str, Any]:
    from .format import HEADER_SIZE, HEADER_STRUCT, crc32_header
    from .meta_extra import book_id_from_extra
    from .reader import open_ebook

    book = open_ebook(path)
    raw = path.read_bytes()[:HEADER_SIZE]
    fields = HEADER_STRUCT.unpack(raw)
    flags = fields[2]
    # CRC 对 Header 前 56 字节；crc32_header 内部已取 [:56]
    return {
        "path": str(path),
        "title": book.meta.title,
        "author": book.meta.author,
        "book_id": book_id_from_extra(book.meta.extra),
        "version": book.version,
        "flags": flags,
        "has_cover": bool(flags & FLAG_HAS_COVER),
        "has_toc": bool(flags & FLAG_HAS_TOC),
        "img_a2i1": bool(flags & FLAG_IMG_A2I1),
        "page_images": bool(flags & FLAG_PAGE_IMAGES),
        "crc_ok": book.crc_ok,
        "chapters": [
            {"index": c.index, "title": c.title, "blocks": c.block_count} for c in book.chapters
        ],
        "chapter_count": len(book.chapters),
        "chunk_max": book.chunk_max,
    }
