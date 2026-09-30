#!/usr/bin/env python3
"""ebook Web：转换 + 在线阅读。

用法:
  cd tools/ebook
  python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
  .venv/bin/python web/app.py
  # 浏览器打开 http://127.0.0.1:8765

打包成 exe（Windows）:
  build_exe.bat
  # 双击 dist/ebook-web.exe → 启动服务并打开浏览器
"""

from __future__ import annotations

import io
import os
import re
import sys
import threading
import time
import uuid
import webbrowser
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

from flask import (
    Flask,
    Response,
    jsonify,
    render_template,
    request,
    send_file,
    send_from_directory,
)
from werkzeug.exceptions import RequestEntityTooLarge


def _is_frozen() -> bool:
    return bool(getattr(sys, "frozen", False)) and hasattr(sys, "_MEIPASS")


def _bundle_dir() -> Path:
    """只读资源目录（模板/静态/源码包）。frozen 时为 PyInstaller 解压目录。"""
    if _is_frozen():
        return Path(sys._MEIPASS)  # type: ignore[attr-defined]
    return Path(__file__).resolve().parent


def _runtime_root() -> Path:
    """可写数据根目录。frozen 时为 exe 所在目录。"""
    if _is_frozen():
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parents[1]


ROOT = _runtime_root()
BUNDLE = _bundle_dir()
# 开发模式：源码根目录；frozen：_MEIPASS 里已放入 epdbook/
_SRC = ROOT if not _is_frozen() else BUNDLE
if str(_SRC) not in sys.path:
    sys.path.insert(0, str(_SRC))

from epdbook.convert_api import ConvertOptions, convert_file  # noqa: E402
from epdbook.cover_ops import clear_ebook_cover, set_ebook_cover  # noqa: E402
from epdbook.meta_extra import book_id_from_extra  # noqa: E402
from epdbook.format import (  # noqa: E402
    DEFAULT_COVER_MAX_H,
    DEFAULT_COVER_MAX_W,
    DEFAULT_IMAGE_MAX_H,
    DEFAULT_IMAGE_MAX_W,
    DEFAULT_PDF_PAGE_MAX_H,
    DEFAULT_PDF_PAGE_MAX_W,
    DEFAULT_PDF_PAGE_SCALE,
)
from epdbook.image_prep import (  # noqa: E402
    DEFAULT_BINARIZE_METHOD,
    binarize_options_from_args,
)
from epdbook.preview import (  # noqa: E402
    epub_image_count,
    pdf_page_count,
    preview_source_binarize,
)
from epdbook.reader import (  # noqa: E402
    decode_image_to_pil,
    load_chapter,
    load_cover_payload,
    open_ebook,
    pieces_have_content,
)

DATA = ROOT / "data"
UPLOADS = DATA / "uploads"
LIBRARY = DATA / "library"
STAGING = DATA / "staging"
UPLOADS.mkdir(parents=True, exist_ok=True)
LIBRARY.mkdir(parents=True, exist_ok=True)
STAGING.mkdir(parents=True, exist_ok=True)

_WEB_DIR = BUNDLE if _is_frozen() else Path(__file__).resolve().parent

# 墨水屏模拟器 .ef 字体：打包在 web/static/fonts/ef；开发模式回退 tools/epdfont/MiSans-Light
_EF_BUNDLED = _WEB_DIR / "static" / "fonts" / "ef"
_EF_DEV = ROOT.parent / "epdfont" / "MiSans-Light"


def _resolve_fonts_ef_dir() -> Path:
    if _EF_BUNDLED.is_dir() and any(_EF_BUNDLED.glob("*.ef")):
        return _EF_BUNDLED
    return _EF_DEV


FONTS_EF_DIR = _resolve_fonts_ef_dir()
# 优先打包在 static/fonts；开发模式回退 tools/fontpack
_FONTPACK_CANDIDATES = (
    _WEB_DIR / "static" / "fonts" / "fonts_misans_25_30.fontpack",
    ROOT.parent / "fontpack" / "fonts" / "MiSans-Mixed" / "fonts_misans_25_30.fontpack",
    ROOT.parent / "fontpack" / "fonts_misans_25_30.fontpack",
    ROOT.parent / "fontpack" / "fonts.fontpack",
)


def _resolve_fontpack() -> Path:
    for p in _FONTPACK_CANDIDATES:
        if p.is_file():
            return p
    return _FONTPACK_CANDIDATES[0]


FONTPACK_FILE = _resolve_fontpack()
WASM_DIR = _WEB_DIR / "static" / "wasm"

app = Flask(
    __name__,
    template_folder=str(_WEB_DIR / "templates"),
    static_folder=str(_WEB_DIR / "static"),
)
MAX_UPLOAD_BYTES = 512 * 1024 * 1024
MAX_UPLOAD_MB = MAX_UPLOAD_BYTES // (1024 * 1024)
app.config["MAX_CONTENT_LENGTH"] = MAX_UPLOAD_BYTES


@app.errorhandler(RequestEntityTooLarge)
def _handle_upload_too_large(_e: RequestEntityTooLarge) -> tuple[Response, int]:
    return jsonify(
        {
            "ok": False,
            "error": f"文件超过上传上限（{MAX_UPLOAD_MB} MB），请拆分源书或使用 convert_ebook.py 命令行转换",
        }
    ), 413

_SAFE = re.compile(r"[^\w\u4e00-\u9fff\-_.]+", re.UNICODE)
_JOB_LOCK = threading.Lock()
_JOBS: dict[str, dict[str, Any]] = {}
_STAGE_LOCK = threading.Lock()
_STAGES: dict[str, dict[str, Any]] = {}


def _parse_binarize_form(form) -> dict[str, Any]:
    method = (form.get("binarize_method") or form.get("binarize") or DEFAULT_BINARIZE_METHOD).strip()
    try:
        threshold = int(form.get("binarize_threshold") or form.get("threshold") or 128)
    except ValueError:
        threshold = 128
    try:
        contrast = float(form.get("binarize_contrast") or form.get("contrast") or 1.0)
    except ValueError:
        contrast = 1.0
    try:
        window = int(form.get("binarize_window") or form.get("sauvola_window") or 25)
    except ValueError:
        window = 25
    try:
        k = float(form.get("binarize_k") or form.get("sauvola_k") or 0.34)
    except ValueError:
        k = 0.34
    return {
        "binarize_method": method,
        "binarize_threshold": threshold,
        "binarize_contrast": contrast,
        "binarize_window": window,
        "binarize_k": k,
    }


def _pil_png_response(im, *, max_w: int = 0, max_h: int = 0) -> Response:
    """JPEG/PNG 彩色原样输出；A2I1/灰度仍转 PNG 灰度。可选缩放到框内。"""
    if max_w > 0 or max_h > 0:
        from PIL import Image as _PILImage

        tw = max_w if max_w > 0 else 4096
        th = max_h if max_h > 0 else 4096
        im = im.copy()
        resample = getattr(_PILImage, "Resampling", _PILImage).LANCZOS
        im.thumbnail((tw, th), resample=resample)
    buf = io.BytesIO()
    if im.mode in ("RGB", "RGBA"):
        if im.mode == "RGBA":
            im = im.convert("RGB")
        im.save(buf, format="PNG")
    else:
        im.convert("L").save(buf, format="PNG")
    buf.seek(0)
    return send_file(buf, mimetype="image/png")


def _parse_thumb_box() -> tuple[int, int]:
    """解析 ?w=&h= 缩略框；非法或未传则 (0,0)=原图。"""
    try:
        w = int(request.args.get("w") or 0)
    except ValueError:
        w = 0
    try:
        h = int(request.args.get("h") or 0)
    except ValueError:
        h = 0
    if w < 0 or h < 0 or w > 1024 or h > 1024:
        return 0, 0
    return w, h


def _cleanup_stages(max_age: float = 7200.0) -> None:
    now = time.time()
    with _STAGE_LOCK:
        stale = [k for k, v in _STAGES.items() if now - v.get("created", 0) > max_age]
        for k in stale:
            meta = _STAGES.pop(k, None)
            if not meta:
                continue
            p = Path(meta.get("path") or "")
            try:
                if p.is_file():
                    p.unlink()
            except OSError:
                pass


def _stage_get(upload_id: str) -> Optional[dict[str, Any]]:
    with _STAGE_LOCK:
        return _STAGES.get(upload_id)


@dataclass
class _Job:
    id: str
    status: str = "queued"  # queued|running|done|error
    percent: int = 0
    message: str = "排队中…"
    error: str = ""
    result: Optional[dict[str, Any]] = None
    created: float = field(default_factory=time.time)


def _safe_name(name: str) -> str:
    stem = Path(name).stem
    stem = _SAFE.sub("_", stem).strip("._") or "book"
    return stem[:80]


def _job_snapshot(job: dict[str, Any]) -> dict[str, Any]:
    out = {
        "ok": True,
        "job_id": job["id"],
        "status": job["status"],
        "percent": job["percent"],
        "message": job["message"],
    }
    if job["status"] == "error":
        out["ok"] = False
        out["error"] = job.get("error") or "转换失败"
    if job["status"] == "done" and job.get("result"):
        out["result"] = job["result"]
    return out


def _set_progress(job_id: str, pct: int, msg: str) -> None:
    with _JOB_LOCK:
        job = _JOBS.get(job_id)
        if not job:
            return
        job["percent"] = max(0, min(100, int(pct)))
        job["message"] = msg
        job["status"] = "running"


def _run_convert_job(
    job_id: str,
    src: Path,
    out: Path,
    opts: ConvertOptions,
) -> None:
    try:
        with _JOB_LOCK:
            if job_id in _JOBS:
                _JOBS[job_id]["status"] = "running"
                _JOBS[job_id]["message"] = "开始转换…"
                _JOBS[job_id]["percent"] = 1

        def on_progress(pct: int, msg: str) -> None:
            _set_progress(job_id, pct, msg)

        result = convert_file(src, out, opts, progress=on_progress)
        payload = {
            "id": result.output.stem,
            "file": result.output.name,
            "title": result.title,
            "author": result.author,
            "book_id": result.book_id,
            "chapters": result.chapters,
            "has_toc": result.has_toc,
            "has_cover": result.has_cover,
            "source_format": result.source_format,
            "warnings": result.warnings,
            "extra": result.extra,
        }
        with _JOB_LOCK:
            job = _JOBS.get(job_id)
            if job:
                job["status"] = "done"
                job["percent"] = 100
                job["message"] = "完成"
                job["result"] = payload
    except Exception as e:
        with _JOB_LOCK:
            job = _JOBS.get(job_id)
            if job:
                job["status"] = "error"
                job["error"] = str(e)
                job["message"] = "失败"
                job["percent"] = max(job.get("percent") or 0, 0)


def _library_index() -> list[dict]:
    rows = []
    for p in sorted(LIBRARY.glob("*.ebook"), key=lambda x: x.stat().st_mtime, reverse=True):
        try:
            book = open_ebook(p)
            st = p.stat()
            rows.append(
                {
                    "id": p.stem,
                    "file": p.name,
                    "title": book.meta.title or p.stem,
                    "author": book.meta.author or "",
                    "book_id": book_id_from_extra(book.meta.extra),
                    "chapters": len(book.chapters),
                    "has_toc": book.has_toc,
                    "has_cover": book.has_cover,
                    "page_images": book.page_images,
                    "size": st.st_size,
                    "mtime": int(st.st_mtime),
                }
            )
        except Exception as e:
            rows.append(
                {
                    "id": p.stem,
                    "file": p.name,
                    "title": p.stem,
                    "author": "",
                    "chapters": 0,
                    "has_toc": False,
                    "has_cover": False,
                    "size": p.stat().st_size,
                    "error": str(e),
                }
            )
    return rows


@app.get("/")
def index():
    return render_template("index.html")


@app.get("/api/config")
def api_config():
    return jsonify({
        "max_upload_mb": MAX_UPLOAD_MB,
        "epd_width": 480,
        "epd_height": 800,
        "wasm_ready": (WASM_DIR / "ebook_emulator.js").is_file(),
    })


@app.get("/api/fonts/ef")
def api_fonts_ef():
    """列出可用 .ef 正文字体（MiSans-Light 等）。"""
    items = []
    if FONTS_EF_DIR.is_dir():
        for p in sorted(FONTS_EF_DIR.glob("*.ef")):
            items.append({"id": p.name, "name": p.stem, "size": p.stat().st_size})
    default = "misans_25_2.ef" if any(x["id"] == "misans_25_2.ef" for x in items) else (
        items[0]["id"] if items else ""
    )
    return jsonify({"ok": True, "fonts": items, "default": default})


@app.get("/api/fonts/ef/<path:name>")
def api_font_ef_file(name: str):
    path = (FONTS_EF_DIR / Path(name).name).resolve()
    if not str(path).startswith(str(FONTS_EF_DIR.resolve())) or not path.is_file():
        return jsonify({"error": "not found"}), 404
    return send_file(path, mimetype="application/octet-stream", max_age=0)


@app.get("/api/fonts/fontpack")
def api_font_fontpack():
    if not FONTPACK_FILE.is_file():
        return jsonify({"ok": False, "error": "fontpack 未找到"}), 404
    return send_file(FONTPACK_FILE, mimetype="application/octet-stream", max_age=3600)


@app.get("/wasm/<path:filename>")
def wasm_static(filename: str):
    if not WASM_DIR.is_dir():
        return jsonify({"error": "wasm not built"}), 404
    return send_from_directory(WASM_DIR, filename)


@app.get("/api/library")
def api_library():
    return jsonify({"books": _library_index()})


@app.post("/api/library/open")
def api_library_open():
    """上传已有 .ebook 到书库，校验后可直接进入阅读页。"""
    f = request.files.get("file")
    if f is None or not f.filename:
        return jsonify({"ok": False, "error": "请选择 .ebook 文件"}), 400

    orig = Path(f.filename).name
    ext = Path(orig).suffix.lower()
    if ext not in (".ebook",):
        return jsonify({"ok": False, "error": "仅支持 .ebook 文件"}), 400

    safe = _safe_name(orig)
    dest = LIBRARY / f"{safe}.ebook"
    if dest.exists():
        dest = LIBRARY / f"{safe}-{uuid.uuid4().hex[:8]}.ebook"

    tmp = dest.with_suffix(dest.suffix + ".tmp")
    try:
        f.save(tmp)
        book = open_ebook(tmp)
        tmp.replace(dest)
    except Exception as e:
        try:
            if tmp.is_file():
                tmp.unlink()
        except OSError:
            pass
        try:
            if dest.is_file():
                dest.unlink()
        except OSError:
            pass
        return jsonify({"ok": False, "error": f"无法打开 .ebook: {e}"}), 400

    book_id = dest.stem
    return jsonify(
        {
            "ok": True,
            "id": book_id,
            "file": dest.name,
            "title": book.meta.title or book_id,
            "author": book.meta.author or "",
            "book_id": book_id_from_extra(book.meta.extra),
            "chapters": len(book.chapters),
            "has_toc": book.has_toc,
            "has_cover": book.has_cover,
        }
    )


@app.post("/api/stage")
def api_stage():
    """上传暂存，供预览与后续转换共用。"""
    _cleanup_stages()
    f = request.files.get("file")
    if f is None or not f.filename:
        return jsonify({"ok": False, "error": "请上传文件"}), 400

    orig = Path(f.filename).name
    uid = uuid.uuid4().hex
    safe = _safe_name(orig)
    ext = Path(orig).suffix.lower()
    dest = STAGING / f"{safe}-{uid[:8]}{ext}"
    f.save(dest)

    fmt = ext.lstrip(".")
    pages = 0
    if fmt == "pdf":
        try:
            pages = pdf_page_count(dest)
        except Exception as e:
            try:
                dest.unlink()
            except OSError:
                pass
            return jsonify({"ok": False, "error": f"无法打开 PDF: {e}"}), 400
    elif fmt == "epub":
        try:
            pages = epub_image_count(dest)
        except Exception as e:
            try:
                dest.unlink()
            except OSError:
                pass
            return jsonify({"ok": False, "error": f"无法打开 EPUB: {e}"}), 400

    meta = {
        "id": uid,
        "path": str(dest),
        "name": orig,
        "format": fmt,
        "pages": pages,
        "created": time.time(),
    }
    with _STAGE_LOCK:
        _STAGES[uid] = meta

    return jsonify(
        {
            "ok": True,
            "upload_id": uid,
            "filename": orig,
            "format": fmt,
            "pages": pages,
            "unit": "pages" if fmt == "pdf" else ("images" if fmt == "epub" else ""),
        }
    )


@app.post("/api/preview-binarize")
def api_preview_binarize():
    """PDF 页 / EPUB 插图：原色 / 灰度 / 二值预览。"""
    form = request.form
    upload_id = (form.get("upload_id") or "").strip()
    meta = _stage_get(upload_id) if upload_id else None
    if not meta:
        return jsonify({"ok": False, "error": "请先选择并上传 PDF/EPUB"}), 400
    path = Path(meta["path"])
    if not path.is_file():
        return jsonify({"ok": False, "error": "暂存文件已失效，请重新选择"}), 400
    fmt = (meta.get("format") or "").lower()
    if fmt not in ("pdf", "epub"):
        return jsonify({"ok": False, "error": "目前仅支持 PDF / EPUB 预览二值化"}), 400

    try:
        page = int(form.get("page") or 0)
    except ValueError:
        page = 0
    try:
        scale = float(form.get("pdf_page_scale") or DEFAULT_PDF_PAGE_SCALE)
    except ValueError:
        scale = DEFAULT_PDF_PAGE_SCALE

    bz = _parse_binarize_form(form)
    try:
        opts = binarize_options_from_args(
            method=bz["binarize_method"],
            threshold=bz["binarize_threshold"],
            contrast=bz["binarize_contrast"],
            window=bz["binarize_window"],
            k=bz["binarize_k"],
        )
        result = preview_source_binarize(
            path,
            fmt=fmt,
            index=max(0, page),
            scale=scale,
            binarize=opts,
        )
        result["format"] = fmt
        return jsonify(result)
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 400


@app.post("/api/convert")
def api_convert():
    form = request.form
    upload_id = (form.get("upload_id") or "").strip()
    src: Optional[Path] = None
    orig_name = ""

    if upload_id:
        meta = _stage_get(upload_id)
        if not meta:
            return jsonify({"ok": False, "error": "暂存已失效，请重新选择文件"}), 400
        staged = Path(meta["path"])
        if not staged.is_file():
            return jsonify({"ok": False, "error": "暂存文件丢失，请重新选择"}), 400
        orig_name = meta.get("name") or staged.name
        # 拷到 uploads，避免转换中 staging 被清理
        uid = uuid.uuid4().hex[:8]
        safe = _safe_name(orig_name)
        src = UPLOADS / f"{safe}-{uid}{Path(orig_name).suffix.lower()}"
        src.write_bytes(staged.read_bytes())
    else:
        f = request.files.get("file")
        if f is None or not f.filename:
            return jsonify({"ok": False, "error": "请上传文件"}), 400
        orig_name = Path(f.filename).name
        uid = uuid.uuid4().hex[:8]
        safe = _safe_name(orig_name)
        src = UPLOADS / f"{safe}-{uid}{Path(orig_name).suffix.lower()}"
        f.save(src)

    pages_as_images = form.get("pdf_pages_as_images") in ("1", "true", "on", "yes")
    try:
        page_scale = float(form.get("pdf_page_scale") or DEFAULT_PDF_PAGE_SCALE)
    except ValueError:
        page_scale = DEFAULT_PDF_PAGE_SCALE
    page_max = form.get("pdf_page_max") or f"{DEFAULT_PDF_PAGE_MAX_W}x{DEFAULT_PDF_PAGE_MAX_H}"
    image_max = form.get("image_max") or f"{DEFAULT_IMAGE_MAX_W}x{DEFAULT_IMAGE_MAX_H}"
    cover_max = form.get("cover_max") or f"{DEFAULT_COVER_MAX_W}x{DEFAULT_COVER_MAX_H}"
    no_images = form.get("no_images") in ("1", "true", "on", "yes")
    no_cover = form.get("no_cover") in ("1", "true", "on", "yes")
    ignore_toc = form.get("ignore_pdf_toc") in ("1", "true", "on", "yes")
    keep_color = form.get("keep_color") in ("1", "true", "on", "yes")
    bz = _parse_binarize_form(form)

    cover_image_path: Optional[Path] = None
    cover_upload = request.files.get("cover_file")
    if cover_upload is not None and cover_upload.filename:
        if no_cover:
            return jsonify({"ok": False, "error": "「不要封面」与自定义封面不能同时使用"}), 400
        uid = uuid.uuid4().hex[:8]
        ext = Path(cover_upload.filename).suffix.lower() or ".jpg"
        if ext not in (".jpg", ".jpeg", ".png", ".webp", ".bmp", ".gif", ".tif", ".tiff"):
            return jsonify({"ok": False, "error": f"不支持的封面格式: {ext}"}), 400
        cover_image_path = UPLOADS / f"cover-{uid}{ext}"
        cover_upload.save(cover_image_path)

    out = LIBRARY / f"{_safe_name(orig_name)}.ebook"
    if out.exists():
        out = LIBRARY / f"{_safe_name(orig_name)}-{uuid.uuid4().hex[:8]}.ebook"

    opts = ConvertOptions(
        title=(form.get("title") or "").strip(),
        author=(form.get("author") or "").strip(),
        book_id=(form.get("book_id") or "").strip(),
        no_images=no_images,
        no_cover=no_cover,
        cover_image=cover_image_path,
        image_max=image_max,
        cover_max=cover_max,
        ignore_pdf_toc=ignore_toc,
        pdf_pages_as_images=pages_as_images,
        pdf_page_scale=page_scale,
        pdf_page_max=page_max,
        binarize_method=bz["binarize_method"],
        binarize_threshold=bz["binarize_threshold"],
        binarize_contrast=bz["binarize_contrast"],
        binarize_window=bz["binarize_window"],
        binarize_k=bz["binarize_k"],
        keep_color=keep_color,
    )

    job_id = uuid.uuid4().hex
    with _JOB_LOCK:
        _JOBS[job_id] = {
            "id": job_id,
            "status": "queued",
            "percent": 0,
            "message": "已上传，排队中…",
            "error": "",
            "result": None,
            "created": time.time(),
        }
        # 清理超过 2 小时的旧任务
        stale = [k for k, v in _JOBS.items() if time.time() - v.get("created", 0) > 7200]
        for k in stale:
            _JOBS.pop(k, None)

    threading.Thread(
        target=_run_convert_job,
        args=(job_id, src, out, opts),
        daemon=True,
        name=f"convert-{job_id[:8]}",
    ).start()

    return jsonify({"ok": True, "job_id": job_id, "status": "queued", "percent": 0, "message": "已上传，排队中…"})


@app.get("/api/convert/<job_id>")
def api_convert_status(job_id: str):
    with _JOB_LOCK:
        job = _JOBS.get(job_id)
        if not job:
            return jsonify({"ok": False, "error": "任务不存在或已过期"}), 404
        return jsonify(_job_snapshot(job))


@app.get("/api/book/<book_id>")
def api_book(book_id: str):
    path = LIBRARY / f"{book_id}.ebook"
    if not path.is_file():
        return jsonify({"ok": False, "error": "not found"}), 404
    book = open_ebook(path)
    chapters = []
    for c in book.chapters:
        pieces = load_chapter(book, c.index)
        if pieces_have_content(pieces):
            chapters.append({"index": c.index, "title": c.title, "flags": c.flags})
    return jsonify(
        {
            "ok": True,
            "id": book_id,
            "title": book.meta.title,
            "author": book.meta.author,
            "book_id": book_id_from_extra(book.meta.extra),
            "has_toc": book.has_toc,
            "has_cover": book.has_cover,
            "page_images": book.page_images,
            "chunk_max": book.chunk_max,
            "chapters": chapters,
        }
    )


@app.get("/api/book/<book_id>/cover")
def api_cover(book_id: str):
    path = LIBRARY / f"{book_id}.ebook"
    if not path.is_file():
        return jsonify({"error": "not found"}), 404
    book = open_ebook(path)
    if not book.has_cover:
        return jsonify({"error": "no cover"}), 404
    piece = load_cover_payload(book)
    if not piece:
        return jsonify({"error": "cover empty"}), 404
    im = decode_image_to_pil(piece)
    tw, th = _parse_thumb_box()
    return _pil_png_response(im, max_w=tw, max_h=th)


@app.post("/api/book/<book_id>/cover")
def api_set_cover(book_id: str):
    """上传图片替换/注入 .ebook 独立封面（A2I1，与转换侧 prepare_cover 同路径）。"""
    path = LIBRARY / f"{book_id}.ebook"
    if not path.is_file():
        return jsonify({"ok": False, "error": "not found"}), 404
    f = request.files.get("cover_file") or request.files.get("file")
    if f is None or not f.filename:
        return jsonify({"ok": False, "error": "请上传封面图片"}), 400
    form = request.form
    cover_max = form.get("cover_max") or f"{DEFAULT_COVER_MAX_W}x{DEFAULT_COVER_MAX_H}"
    keep_color = form.get("keep_color") in ("1", "true", "on", "yes")
    bz = _parse_binarize_form(form)
    uid = uuid.uuid4().hex[:8]
    ext = Path(f.filename).suffix.lower() or ".jpg"
    tmp = UPLOADS / f"cover-set-{uid}{ext}"
    try:
        f.save(tmp)
        result = set_ebook_cover(
            path,
            tmp,
            cover_max=cover_max,
            binarize=binarize_options_from_args(
                method=bz["binarize_method"],
                threshold=bz["binarize_threshold"],
                contrast=bz["binarize_contrast"],
                window=bz["binarize_window"],
                k=bz["binarize_k"],
            ),
            keep_color=keep_color,
        )
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 400
    finally:
        try:
            if tmp.is_file():
                tmp.unlink()
        except OSError:
            pass
    return jsonify(
        {
            "ok": True,
            "id": book_id,
            "has_cover": result.has_cover,
            "width": result.width,
            "height": result.height,
            "fmt": result.fmt,
            "cover_size": result.cover_size,
            "mtime": int(path.stat().st_mtime),
        }
    )


@app.delete("/api/book/<book_id>/cover")
def api_clear_cover(book_id: str):
    path = LIBRARY / f"{book_id}.ebook"
    if not path.is_file():
        return jsonify({"ok": False, "error": "not found"}), 404
    try:
        result = clear_ebook_cover(path)
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 400
    return jsonify(
        {
            "ok": True,
            "id": book_id,
            "has_cover": result.has_cover,
            "mtime": int(path.stat().st_mtime),
        }
    )


@app.get("/api/book/<book_id>/chapter/<int:index>")
def api_chapter(book_id: str, index: int):
    path = LIBRARY / f"{book_id}.ebook"
    if not path.is_file():
        return jsonify({"ok": False, "error": "not found"}), 404
    book = open_ebook(path)
    if index < 0 or index >= len(book.chapters):
        return jsonify({"ok": False, "error": "chapter out of range"}), 400
    pieces = load_chapter(book, index)
    out = []
    for i, p in enumerate(pieces):
        if p.kind == "text":
            out.append({"kind": "text", "index": i, "text": p.text})
        else:
            out.append(
                {
                    "kind": "image",
                    "index": i,
                    "fmt": p.image_fmt,
                    "width": p.width,
                    "height": p.height,
                    "url": f"/api/book/{book_id}/chapter/{index}/image/{i}",
                }
            )
    ch = book.chapters[index]
    return jsonify({"ok": True, "index": index, "title": ch.title, "pieces": out})


@app.get("/api/book/<book_id>/chapter/<int:index>/image/<int:piece>")
def api_chapter_image(book_id: str, index: int, piece: int):
    path = LIBRARY / f"{book_id}.ebook"
    if not path.is_file():
        return Response("not found", 404)
    book = open_ebook(path)
    pieces = load_chapter(book, index)
    if piece < 0 or piece >= len(pieces) or pieces[piece].kind != "image":
        return Response("not found", 404)
    im = decode_image_to_pil(pieces[piece])
    return _pil_png_response(im)


@app.get("/api/book/<book_id>/download")
def api_download(book_id: str):
    path = LIBRARY / f"{book_id}.ebook"
    if not path.is_file():
        return jsonify({"error": "not found"}), 404
    # 强制附件下载，文件名带 .ebook
    return send_file(
        path,
        as_attachment=True,
        download_name=path.name,
        mimetype="application/octet-stream",
        max_age=0,
    )


@app.delete("/api/book/<book_id>")
def api_delete(book_id: str):
    path = LIBRARY / f"{book_id}.ebook"
    if path.is_file():
        path.unlink()
    return jsonify({"ok": True})


@app.delete("/api/library")
def api_clear_library():
    """一键删除书库内全部 .ebook。"""
    removed = 0
    for p in list(LIBRARY.glob("*.ebook")):
        try:
            p.unlink()
            removed += 1
        except OSError:
            pass
    return jsonify({"ok": True, "removed": removed})


def main() -> None:
    import socket

    host = os.environ.get("EBOOK_HOST", "127.0.0.1")
    port = int(os.environ.get("EBOOK_PORT", os.environ.get("PORT", "8765")))

    def _env_true(name: str) -> bool:
        return os.environ.get(name, "").strip().lower() in ("1", "true", "yes", "on")

    # 双击 exe 默认打开浏览器；源码启动默认不抢浏览器（与 ./run_web.sh 一致）
    if _is_frozen():
        open_browser = not _env_true("EBOOK_NO_BROWSER")
    else:
        open_browser = _env_true("EBOOK_OPEN_BROWSER")

    def _port_free(p: int) -> bool:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                s.bind((host, p))
                return True
            except OSError:
                return False

    if not _port_free(port):
        # 自动顺延找空闲端口（避免「Address already in use」直接挂掉）
        base = port
        for p in range(base, base + 20):
            if _port_free(p):
                print(
                    f"提示: 端口 {base} 已被占用，改用 {p}",
                    file=sys.stderr,
                )
                port = p
                break
        else:
            print(
                f"错误: {base}–{base + 19} 均被占用。可执行: "
                f"EBOOK_PORT=9000 ./run_web.sh  或  fuser -k {base}/tcp",
                file=sys.stderr,
            )
            raise SystemExit(1)

    url = f"http://{host}:{port}"
    print(f"ebook web → {url}")
    print(f"数据目录 → {DATA}")
    print("关闭本窗口即停止服务。")

    if open_browser:
        def _open() -> None:
            try:
                webbrowser.open(url)
            except Exception as e:
                print(f"无法自动打开浏览器: {e}", file=sys.stderr)

        threading.Timer(0.8, _open).start()

    # use_reloader=False：打包后 / 双击启动不可热重载
    app.run(host=host, port=port, debug=False, threaded=True, use_reloader=False)


if __name__ == "__main__":
    main()
