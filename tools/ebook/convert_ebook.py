#!/usr/bin/env python3
"""将 txt / pdf / epub / mobi 转为 ESP32 友好的 .ebook（CLI）。

图片策略（墨水屏）：全部 A2I1；扫描 PDF 可用 --pdf-pages-as-images。
共享逻辑见 epdbook.convert_api；封面补丁见 epdbook.cover_ops；Web 端见 web/app.py。

用法:
  convert_ebook.py book.txt -o out.ebook [--cover cover.jpg]
  convert_ebook.py set-cover book.ebook cover.jpg
  convert_ebook.py clear-cover book.ebook
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parent
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from epdbook import __version__
from epdbook.convert_api import ConvertOptions, convert_file, inspect_ebook
from epdbook.cover_ops import clear_ebook_cover, cover_info, set_ebook_cover
from epdbook.format import (
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
    DEVICE_CHUNK_MAX,
)
from epdbook.image_prep import BINARIZE_METHODS, DEFAULT_BINARIZE_METHOD


def _add_binarize_args(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--binarize",
        choices=BINARIZE_METHODS,
        default=DEFAULT_BINARIZE_METHOD,
        help=f"A2I1 二值化：fixed|otsu|sauvola|dither|bayer（默认 {DEFAULT_BINARIZE_METHOD}）",
    )
    p.add_argument("--threshold", type=int, default=128, help="fixed 阈值 0–255")
    p.add_argument("--contrast", type=float, default=1.0, help="对比度增强（1.0=不变）")
    p.add_argument("--sauvola-window", type=int, default=25, help="sauvola 窗口（奇数）")
    p.add_argument("--sauvola-k", type=float, default=0.34, help="sauvola k")
    p.add_argument(
        "--keep-color",
        action="store_true",
        help="保留彩色 JPEG，不二值化；设备阅读时按原生 EPUB 同款 Bayer 转黑白",
    )


def _add_cover_encode_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("--cover-max", default=f"{DEFAULT_COVER_MAX_W}x{DEFAULT_COVER_MAX_H}")
    p.add_argument("--cover-max-payload", type=int, default=COVER_MAX_PAYLOAD)
    _add_binarize_args(p)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Convert books to ultra-light .ebook for ESP32-S3 (A2I1 images)",
    )
    p.add_argument("--version", action="version", version=f"ebook {__version__}")
    sub = p.add_subparsers(dest="command")

    # ---- convert（默认；兼容无子命令）----
    c = sub.add_parser("convert", help="源文件 → .ebook（默认命令）")
    _add_convert_args(c)

    sc = sub.add_parser("set-cover", help="替换/注入已有 .ebook 的封面")
    sc.add_argument("ebook", type=Path, help=".ebook 路径")
    sc.add_argument("image", type=Path, help="封面图片（jpg/png/…）")
    _add_cover_encode_args(sc)

    cc = sub.add_parser("clear-cover", help="清除已有 .ebook 的独立封面")
    cc.add_argument("ebook", type=Path, help=".ebook 路径")

    ic = sub.add_parser("cover-info", help="查看 .ebook 封面元数据")
    ic.add_argument("ebook", type=Path, help=".ebook 路径")

    return p


def _add_convert_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("input", type=Path, help="源文件 txt/pdf/epub/mobi")
    p.add_argument("-o", "--output", type=Path, help="输出 .ebook 路径")
    p.add_argument("--format", choices=("txt", "pdf", "epub", "mobi"), help="强制输入格式")
    p.add_argument("--title", default="", help="覆盖书名")
    p.add_argument("--author", default="", help="覆盖作者")
    p.add_argument(
        "--book-id",
        default="",
        help="写入 Metadata.extra.book_id（空则自动生成 uuid 去横线）",
    )
    p.add_argument("--lang", default="zh")
    p.add_argument("--font-px", type=int, default=DEFAULT_FONT_PX)
    p.add_argument(
        "--chunk-max",
        type=int,
        default=DEFAULT_CHUNK_MAX,
        help=f"正文单块解压上限（默认 {DEFAULT_CHUNK_MAX}，设备硬上限 {DEVICE_CHUNK_MAX}）",
    )
    p.add_argument("--chapter-regex", action="append", default=None)
    p.add_argument("--no-split", action="store_true")
    p.add_argument(
        "--chapter-max-bytes",
        type=int,
        default=DEFAULT_CHAPTER_MAX_BYTES,
        help=f"单章解压合计上限（文本+图），超出则自动拆章（默认 {DEFAULT_CHAPTER_MAX_BYTES}）",
    )
    p.add_argument(
        "--no-chapter-size-split",
        action="store_true",
        help="禁用超大章按体积拆分（可能导致设备打开 OOM；仅调试用）",
    )
    p.add_argument("--no-images", action="store_true")
    p.add_argument("--no-cover", action="store_true")
    p.add_argument(
        "--cover",
        type=Path,
        default=None,
        help="自定义封面图（TXT 注入 / 覆盖源封面；与 --no-cover 互斥）",
    )
    p.add_argument(
        "--image-max",
        default=f"{DEFAULT_IMAGE_MAX_W}x{DEFAULT_IMAGE_MAX_H}",
        help=f"插图最大框 WxH，或 orig=保留原始尺寸（默认 {DEFAULT_IMAGE_MAX_W}x{DEFAULT_IMAGE_MAX_H}）",
    )
    p.add_argument("--cover-max", default=f"{DEFAULT_COVER_MAX_W}x{DEFAULT_COVER_MAX_H}")
    p.add_argument("--cover-max-payload", type=int, default=COVER_MAX_PAYLOAD)
    p.add_argument("--no-font-heuristic", action="store_true")
    p.add_argument("--ignore-pdf-toc", action="store_true")
    p.add_argument("--pdf-toc-max-level", type=int, default=None, metavar="N")
    p.add_argument("--force-heuristic", action="store_true")
    p.add_argument(
        "--pdf-pages-as-images",
        action="store_true",
        help="扫描 PDF：每页光栅化为图片写入（无文字层时用）",
    )
    p.add_argument(
        "--pdf-page-scale",
        type=float,
        default=DEFAULT_PDF_PAGE_SCALE,
        help=f"整页渲染倍率（相对 72dpi，默认 {DEFAULT_PDF_PAGE_SCALE}）",
    )
    p.add_argument(
        "--pdf-page-max",
        default=f"{DEFAULT_PDF_PAGE_MAX_W}x{DEFAULT_PDF_PAGE_MAX_H}",
        help=f"整页图最大框（默认 {DEFAULT_PDF_PAGE_MAX_W}x{DEFAULT_PDF_PAGE_MAX_H}）",
    )
    _add_binarize_args(p)
    p.add_argument("--inspect", action="store_true")


def _parse_argv(argv: list[str] | None) -> argparse.Namespace:
    """无子命令时默认走 convert，兼容旧用法 convert_ebook.py book.txt -o out.ebook。"""
    argv = list(sys.argv[1:] if argv is None else argv)
    known = {"convert", "set-cover", "clear-cover", "cover-info", "-h", "--help", "--version"}
    if not argv or argv[0] not in known:
        # 旧式：直接跟 input 路径
        if argv and not argv[0].startswith("-"):
            argv = ["convert"] + argv
        elif argv and argv[0].startswith("-") and argv[0] not in ("-h", "--help", "--version"):
            argv = ["convert"] + argv
        elif not argv:
            argv = ["convert", "--help"]
    return build_parser().parse_args(argv)


def _cmd_convert(args: argparse.Namespace) -> int:
    if args.no_cover and args.cover:
        print("错误: --no-cover 与 --cover 不能同时使用", file=sys.stderr)
        return 2
    if args.cover and not Path(args.cover).is_file():
        print(f"错误: 封面文件不存在: {args.cover}", file=sys.stderr)
        return 2

    src: Path = args.input
    out = args.output or src.with_suffix(".ebook")
    opts = ConvertOptions(
        title=args.title,
        author=args.author,
        lang=args.lang,
        book_id=args.book_id,
        font_px=args.font_px,
        chunk_max=args.chunk_max,
        chapter_regex=args.chapter_regex,
        no_split=args.no_split,
        chapter_max_bytes=args.chapter_max_bytes,
        no_chapter_size_split=args.no_chapter_size_split,
        no_images=args.no_images,
        no_cover=args.no_cover,
        cover_image=args.cover,
        image_max=args.image_max,
        cover_max=args.cover_max,
        cover_max_payload=args.cover_max_payload,
        no_font_heuristic=args.no_font_heuristic,
        ignore_pdf_toc=args.ignore_pdf_toc,
        pdf_toc_max_level=args.pdf_toc_max_level,
        force_heuristic=args.force_heuristic,
        pdf_pages_as_images=args.pdf_pages_as_images,
        pdf_page_scale=args.pdf_page_scale,
        pdf_page_max=args.pdf_page_max,
        force_format=args.format,
        binarize_method=args.binarize,
        binarize_threshold=args.threshold,
        binarize_contrast=args.contrast,
        binarize_window=args.sauvola_window,
        binarize_k=args.sauvola_k,
        keep_color=args.keep_color,
    )
    try:
        result = convert_file(src, out, opts)
    except Exception as e:
        print(f"转换失败: {e}", file=sys.stderr)
        return 1

    for w in result.warnings:
        print(f"提示: {w}", file=sys.stderr)
    img_mode = "jpeg" if args.keep_color else f"a2i1/{args.binarize}"
    print(
        f"OK → {result.output}  ({result.chapters} chapters, {img_mode}, "
        f"toc={'yes' if result.has_toc else 'none'}, "
        f"cover={'yes' if result.has_cover else 'none'}, "
        f"book_id={result.book_id or 'none'})"
    )
    if args.inspect:
        info = inspect_ebook(result.output)
        print(
            f"  version={info['version']} book_id={info.get('book_id') or '-'} "
            f"flags has_cover={info['has_cover']} "
            f"has_toc={info['has_toc']} chapters={info['chapter_count']} "
            f"chunk_max={info['chunk_max']} crc_ok={info['crc_ok']}"
        )
        for c in info["chapters"][:12]:
            print(f"  [{c['index']}] {c['title']!r} blocks={c['blocks']}")
        if info["chapter_count"] > 12:
            print(f"  … +{info['chapter_count'] - 12} more")
    return 0


def _cmd_set_cover(args: argparse.Namespace) -> int:
    try:
        from epdbook.image_prep import binarize_options_from_args

        bz = binarize_options_from_args(
            method=args.binarize,
            threshold=args.threshold,
            contrast=args.contrast,
            window=args.sauvola_window,
            k=args.sauvola_k,
        )
        result = set_ebook_cover(
            args.ebook,
            args.image,
            cover_max=args.cover_max,
            max_payload=args.cover_max_payload,
            binarize=bz,
            keep_color=args.keep_color,
        )
    except Exception as e:
        print(f"设置封面失败: {e}", file=sys.stderr)
        return 1
    print(
        f"OK set-cover → {result.path}  "
        f"({result.width}x{result.height} fmt={result.fmt} size={result.cover_size})"
    )
    return 0


def _cmd_clear_cover(args: argparse.Namespace) -> int:
    try:
        result = clear_ebook_cover(args.ebook)
    except Exception as e:
        print(f"清除封面失败: {e}", file=sys.stderr)
        return 1
    print(f"OK clear-cover → {result.path}")
    return 0


def _cmd_cover_info(args: argparse.Namespace) -> int:
    try:
        info = cover_info(args.ebook)
    except Exception as e:
        print(f"读取失败: {e}", file=sys.stderr)
        return 1
    print(
        f"{args.ebook}: has_cover={info.has_cover} crc_ok={info.crc_ok} "
        f"data_end={info.data_end}"
    )
    if info.has_cover:
        print(
            f"  offset={info.offset} size={info.size} "
            f"{info.width}x{info.height} {info.fmt_name}"
        )
    return 0


def main(argv: list[str] | None = None) -> int:
    args = _parse_argv(argv)
    cmd = getattr(args, "command", None) or "convert"
    if cmd == "convert":
        return _cmd_convert(args)
    if cmd == "set-cover":
        return _cmd_set_cover(args)
    if cmd == "clear-cover":
        return _cmd_clear_cover(args)
    if cmd == "cover-info":
        return _cmd_cover_info(args)
    print(f"未知命令: {cmd}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
