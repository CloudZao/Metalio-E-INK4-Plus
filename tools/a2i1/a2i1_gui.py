#!/usr/bin/env python3
"""A2I1 Windows desktop GUI — preview / convert for GDEM0397T81P e-ink."""

from __future__ import annotations

import sys
from pathlib import Path

# Sibling imports when run as script / PyInstaller bundle
if getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS"):
    _HERE = Path(sys._MEIPASS)  # type: ignore[attr-defined]
else:
    _HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from typing import Optional

from PIL import Image, ImageTk

from a2i1_core import (
    PANEL_ACTIVE_PORTRAIT_MM,
    PANEL_BEZEL_BOTTOM_PX,
    PANEL_BEZEL_LEFT_PX,
    PANEL_BEZEL_RIGHT_PX,
    PANEL_BEZEL_TOP_PX,
    PANEL_DIAGONAL_INCH,
    PANEL_LOGICAL_H,
    PANEL_LOGICAL_W,
    PANEL_NAME,
    PANEL_OUTLINE_H_PX,
    PANEL_OUTLINE_PORTRAIT_MM,
    PANEL_OUTLINE_W_PX,
    PANEL_PIXEL_PITCH_MM,
    A2i1Error,
    A2i1Info,
    convert_image,
    load_a2i1,
    parse_size,
    render_panel_module,
    save_a2i1,
)

APP_TITLE = f"A2I1 墨水屏工具 — {PANEL_NAME} {PANEL_DIAGONAL_INCH}\""
BG = "#2b2b2b"
PANEL_BG = "#1e1e1e"
FG = "#e8e8e0"
ACCENT = "#8a9a7b"


class A2i1App(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title(APP_TITLE)
        # 1:1 模组约 520×894 + 右侧面板；默认开大一点减少滚动
        self.geometry("1600x1100")
        self.minsize(1200, 900)
        self.configure(bg=BG)

        self._bw: Optional[Image.Image] = None
        self._info: Optional[A2i1Info] = None
        self._source_path: Optional[Path] = None
        self._photo: Optional[ImageTk.PhotoImage] = None
        self._fit_after: Optional[str] = None
        self._source_rgb: Optional[Image.Image] = None
        self._reconvert_after: Optional[str] = None

        self._build_style()
        self._build_ui()
        self._update_info_labels()
        self._refresh_preview()

    def _build_style(self) -> None:
        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure(".", background=BG, foreground=FG, fieldbackground="#3a3a3a")
        style.configure("TFrame", background=BG)
        style.configure("TLabel", background=BG, foreground=FG)
        style.configure("TLabelframe", background=BG, foreground=FG)
        style.configure("TLabelframe.Label", background=BG, foreground=ACCENT)
        style.configure("TButton", padding=6)
        style.configure("TCheckbutton", background=BG, foreground=FG)
        style.configure("TRadiobutton", background=BG, foreground=FG)
        style.configure("Horizontal.TScale", background=BG)

    def _build_ui(self) -> None:
        toolbar = ttk.Frame(self, padding=(10, 8))
        toolbar.pack(side=tk.TOP, fill=tk.X)
        ttk.Button(toolbar, text="打开 A2I1", command=self.open_a2i1).pack(side=tk.LEFT, padx=4)
        ttk.Button(toolbar, text="打开图片", command=self.open_image).pack(side=tk.LEFT, padx=4)
        ttk.Button(toolbar, text="另存为 A2I1", command=self.save_a2i1_file).pack(side=tk.LEFT, padx=4)
        ttk.Button(toolbar, text="导出预览 PNG", command=self.export_preview_png).pack(side=tk.LEFT, padx=4)

        body = ttk.Frame(self, padding=(10, 0, 10, 10))
        body.pack(side=tk.TOP, fill=tk.BOTH, expand=True)
        body.columnconfigure(0, weight=3)
        body.columnconfigure(1, weight=1)
        body.rowconfigure(0, weight=1)

        preview_frame = ttk.LabelFrame(
            body,
            text=f"墨水屏模组预览（{PANEL_NAME} 竖屏 AA {PANEL_LOGICAL_W}×{PANEL_LOGICAL_H}）",
            padding=8,
        )
        preview_frame.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        preview_frame.rowconfigure(0, weight=1)
        preview_frame.columnconfigure(0, weight=1)

        canvas_wrap = ttk.Frame(preview_frame)
        canvas_wrap.grid(row=0, column=0, sticky="nsew")
        canvas_wrap.rowconfigure(0, weight=1)
        canvas_wrap.columnconfigure(0, weight=1)

        self.canvas = tk.Canvas(canvas_wrap, bg=PANEL_BG, highlightthickness=0)
        self.canvas.grid(row=0, column=0, sticky="nsew")
        yscroll = ttk.Scrollbar(canvas_wrap, orient=tk.VERTICAL, command=self.canvas.yview)
        yscroll.grid(row=0, column=1, sticky="ns")
        xscroll = ttk.Scrollbar(canvas_wrap, orient=tk.HORIZONTAL, command=self.canvas.xview)
        xscroll.grid(row=1, column=0, sticky="ew")
        self.canvas.configure(xscrollcommand=xscroll.set, yscrollcommand=yscroll.set)
        self.canvas.bind("<Configure>", lambda _e: self._schedule_fit())
        self.canvas.bind("<MouseWheel>", self._on_mousewheel)
        self.canvas.bind("<Button-4>", lambda e: self.canvas.yview_scroll(-1, "units"))
        self.canvas.bind("<Button-5>", lambda e: self.canvas.yview_scroll(1, "units"))

        side = ttk.Frame(body)
        side.grid(row=0, column=1, sticky="nsew")

        info = ttk.LabelFrame(side, text="尺寸 / 文件", padding=10)
        info.pack(fill=tk.X, pady=(0, 8))
        self.lbl_size = ttk.Label(info, text="图片尺寸：—")
        self.lbl_size.pack(anchor="w")
        self.lbl_stride = ttk.Label(info, text="stride：—")
        self.lbl_stride.pack(anchor="w")
        self.lbl_bytes = ttk.Label(info, text="文件大小：—")
        self.lbl_bytes.pack(anchor="w")
        self.lbl_panel = ttk.Label(info, text="", wraplength=300)
        self.lbl_panel.pack(anchor="w", pady=(6, 0))
        self.lbl_path = ttk.Label(info, text="路径：—", wraplength=300)
        self.lbl_path.pack(anchor="w", pady=(6, 0))

        conv = ttk.LabelFrame(side, text="转换参数", padding=10)
        conv.pack(fill=tk.X, pady=(0, 8))

        ttk.Label(conv, text="二值化方法").pack(anchor="w")
        self.method_var = tk.StringVar(value="threshold")
        for label, value in (
            ("阈值（线稿/插画）", "threshold"),
            ("Floyd 抖动（照片）", "floyd"),
            ("无抖动量化", "none"),
        ):
            ttk.Radiobutton(
                conv, text=label, value=value, variable=self.method_var, command=self._maybe_reconvert
            ).pack(anchor="w")

        thr_row = ttk.Frame(conv)
        thr_row.pack(fill=tk.X, pady=(8, 0))
        ttk.Label(thr_row, text="阈值").pack(side=tk.LEFT)
        self.threshold_var = tk.IntVar(value=128)
        self.thr_scale = ttk.Scale(
            thr_row,
            from_=0,
            to=255,
            orient=tk.HORIZONTAL,
            command=self._on_threshold,
        )
        self.thr_scale.set(128)
        self.thr_scale.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=6)
        self.lbl_thr = ttk.Label(thr_row, text="128", width=4)
        self.lbl_thr.pack(side=tk.LEFT)

        size_row = ttk.Frame(conv)
        size_row.pack(fill=tk.X, pady=(8, 0))
        self.resize_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(
            size_row,
            text="缩放到面板",
            variable=self.resize_var,
            command=self._maybe_reconvert,
        ).pack(side=tk.LEFT)
        self.size_var = tk.StringVar(value=f"{PANEL_LOGICAL_W}x{PANEL_LOGICAL_H}")
        ttk.Entry(size_row, textvariable=self.size_var, width=12).pack(side=tk.LEFT, padx=6)
        ttk.Label(size_row, text="(宽x高)").pack(side=tk.LEFT)

        ttk.Button(conv, text="重新转换预览", command=self._maybe_reconvert).pack(
            fill=tk.X, pady=(10, 0)
        )

        view = ttk.LabelFrame(side, text="预览显示", padding=10)
        view.pack(fill=tk.X)
        self.soft_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(
            view, text="墨水屏柔边模拟", variable=self.soft_var, command=self._refresh_preview
        ).pack(anchor="w")

        zoom_row = ttk.Frame(view)
        zoom_row.pack(fill=tk.X, pady=(8, 0))
        ttk.Label(zoom_row, text="缩放").pack(side=tk.LEFT)
        self.zoom_mode = tk.StringVar(value="1x")
        ttk.Radiobutton(
            zoom_row, text="1:1", value="1x", variable=self.zoom_mode, command=self._refresh_preview
        ).pack(side=tk.LEFT, padx=4)
        ttk.Radiobutton(
            zoom_row, text="2:1", value="2x", variable=self.zoom_mode, command=self._refresh_preview
        ).pack(side=tk.LEFT)
        ttk.Radiobutton(
            zoom_row, text="适应窗口", value="fit", variable=self.zoom_mode, command=self._refresh_preview
        ).pack(side=tk.LEFT, padx=4)

        tip = ttk.Label(
            side,
            text=(
                f"{PANEL_NAME} 复刻预览\n"
                f"外形 {PANEL_OUTLINE_PORTRAIT_MM[0]:.2f}×{PANEL_OUTLINE_PORTRAIT_MM[1]:.2f} mm\n"
                f"有效区 {PANEL_ACTIVE_PORTRAIT_MM[0]:.2f}×{PANEL_ACTIVE_PORTRAIT_MM[1]:.2f} mm\n"
                f"像素节距 {PANEL_PIXEL_PITCH_MM} mm · {PANEL_LOGICAL_W}×{PANEL_LOGICAL_H}\n"
                f"边框 L/R {PANEL_BEZEL_LEFT_PX}/{PANEL_BEZEL_RIGHT_PX}px "
                f"T/B {PANEL_BEZEL_TOP_PX}/{PANEL_BEZEL_BOTTOM_PX}px\n"
                f"模组预览 {PANEL_OUTLINE_W_PX}×{PANEL_OUTLINE_H_PX}px @1:1\n"
                f"内容从有效区 (0,0) 左上起绘"
            ),
            justify=tk.LEFT,
        )
        tip.pack(anchor="w", pady=12)

    def _on_mousewheel(self, event: tk.Event) -> None:
        delta = -1 if event.delta > 0 else 1
        if event.delta == 0:
            return
        self.canvas.yview_scroll(delta, "units")

    def _on_threshold(self, _val: str) -> None:
        v = int(float(self.thr_scale.get()))
        self.threshold_var.set(v)
        self.lbl_thr.configure(text=str(v))
        if self.method_var.get() == "threshold" and self._source_rgb is not None:
            self._schedule_reconvert()

    def _schedule_reconvert(self) -> None:
        if self._reconvert_after:
            self.after_cancel(self._reconvert_after)
        self._reconvert_after = self.after(180, self._maybe_reconvert)

    def _schedule_fit(self) -> None:
        if self.zoom_mode.get() != "fit":
            return
        if self._fit_after:
            self.after_cancel(self._fit_after)
        self._fit_after = self.after(80, self._refresh_preview)

    def open_a2i1(self) -> None:
        path = filedialog.askopenfilename(
            title="打开 A2I1",
            filetypes=[("A2I1", "*.a2i1"), ("所有文件", "*.*")],
        )
        if not path:
            return
        try:
            info, bw = load_a2i1(path)
        except (OSError, A2i1Error) as exc:
            messagebox.showerror("打开失败", str(exc))
            return
        self._source_rgb = None
        self._source_path = Path(path)
        self._info = info
        self._bw = bw
        self._update_info_labels()
        self._refresh_preview()

    def open_image(self) -> None:
        path = filedialog.askopenfilename(
            title="打开图片",
            filetypes=[
                ("图片", "*.png *.jpg *.jpeg *.bmp *.gif *.webp"),
                ("所有文件", "*.*"),
            ],
        )
        if not path:
            return
        try:
            im = Image.open(path)
            im.load()
            self._source_rgb = im.convert("RGB")
        except OSError as exc:
            messagebox.showerror("打开失败", str(exc))
            return
        self._source_path = Path(path)
        self._maybe_reconvert()

    def save_a2i1_file(self) -> None:
        if self._bw is None:
            messagebox.showinfo("提示", "请先打开或转换图片")
            return
        default = "output.a2i1"
        if self._source_path is not None:
            default = self._source_path.with_suffix(".a2i1").name
        path = filedialog.asksaveasfilename(
            title="另存为 A2I1",
            defaultextension=".a2i1",
            initialfile=default,
            filetypes=[("A2I1", "*.a2i1")],
        )
        if not path:
            return
        try:
            data = save_a2i1(path, self._bw)
            info, _ = load_a2i1(path)
            self._info = info
            self._source_path = Path(path)
            self._update_info_labels()
            messagebox.showinfo("已保存", f"{path}\n{len(data)} 字节，{info.width}×{info.height}")
        except (OSError, A2i1Error, ValueError) as exc:
            messagebox.showerror("保存失败", str(exc))

    def export_preview_png(self) -> None:
        path = filedialog.asksaveasfilename(
            title="导出模组预览 PNG",
            defaultextension=".png",
            initialfile="a2i1_panel_preview.png",
            filetypes=[("PNG", "*.png")],
        )
        if not path:
            return
        try:
            preview = render_panel_module(
                self._bw,
                scale=1.0,
                soft=self.soft_var.get(),
            )
            preview.save(path)
            messagebox.showinfo("已导出", f"{path}\n{preview.size[0]}×{preview.size[1]} @1:1")
        except OSError as exc:
            messagebox.showerror("导出失败", str(exc))

    def _target_size(self) -> Optional[tuple[int, int]]:
        if not self.resize_var.get():
            return None
        try:
            return parse_size(self.size_var.get())
        except ValueError:
            messagebox.showerror("尺寸无效", "请使用如 480x800 的格式")
            return None

    def _maybe_reconvert(self) -> None:
        if self._source_rgb is None:
            self._refresh_preview()
            return
        size = self._target_size()
        if self.resize_var.get() and size is None:
            return
        method = self.method_var.get()  # type: ignore[assignment]
        try:
            bw = convert_image(
                self._source_rgb,
                method=method,
                threshold=self.threshold_var.get(),
                size=size,
            )
        except ValueError as exc:
            messagebox.showerror("转换失败", str(exc))
            return
        self._bw = bw
        stride = (bw.size[0] + 7) // 8
        payload = stride * bw.size[1]
        self._info = A2i1Info(
            width=bw.size[0],
            height=bw.size[1],
            stride=stride,
            reserved=0,
            file_size=20 + payload,
            payload_size=payload,
        )
        self._update_info_labels()
        self._refresh_preview()

    def _update_info_labels(self) -> None:
        panel_txt = (
            f"模组：{PANEL_NAME}\n"
            f"外形 {PANEL_OUTLINE_PORTRAIT_MM[0]:.2f}×{PANEL_OUTLINE_PORTRAIT_MM[1]:.2f} mm "
            f"→ {PANEL_OUTLINE_W_PX}×{PANEL_OUTLINE_H_PX} px\n"
            f"AA {PANEL_ACTIVE_PORTRAIT_MM[0]:.2f}×{PANEL_ACTIVE_PORTRAIT_MM[1]:.2f} mm "
            f"→ {PANEL_LOGICAL_W}×{PANEL_LOGICAL_H} px"
        )
        if self._info is None:
            self.lbl_size.configure(text="图片尺寸：—（空白屏）")
            self.lbl_stride.configure(text="stride：—")
            self.lbl_bytes.configure(text="文件大小：—")
            self.lbl_panel.configure(text=panel_txt)
            self.lbl_path.configure(text="路径：—")
            return
        info = self._info
        self.lbl_size.configure(text=f"图片尺寸：{info.width} × {info.height} px（贴 AA 左上 0,0）")
        self.lbl_stride.configure(text=f"stride：{info.stride} 字节/行")
        self.lbl_bytes.configure(
            text=f"文件大小：{info.file_size} 字节（约 {info.file_size / 1024:.1f} KB）"
        )
        self.lbl_panel.configure(text=f"{panel_txt}\n匹配：{info.panel_hint}")
        path_txt = str(self._source_path) if self._source_path else "（未保存）"
        self.lbl_path.configure(text=f"路径：{path_txt}")

    def _preview_scale(self) -> float:
        mode = self.zoom_mode.get()
        if mode == "1x":
            return 1.0
        if mode == "2x":
            return 2.0
        cw = max(self.canvas.winfo_width(), 100)
        ch = max(self.canvas.winfo_height(), 100)
        sx = (cw - 8) / PANEL_OUTLINE_W_PX
        sy = (ch - 8) / PANEL_OUTLINE_H_PX
        return max(0.05, min(sx, sy, 4.0))

    def _refresh_preview(self) -> None:
        self.canvas.delete("all")
        scale = self._preview_scale()
        preview = render_panel_module(
            self._bw,
            scale=scale,
            soft=self.soft_var.get(),
        )
        self._photo = ImageTk.PhotoImage(preview)
        # 模组整体从画布 (0,0) 起；图内容在 AA 内也是 (0,0)
        self.canvas.create_image(0, 0, image=self._photo, anchor=tk.NW)
        self.canvas.configure(scrollregion=(0, 0, preview.size[0], preview.size[1]))

        if self._bw is None:
            caption = f"空白屏 @{'1:1' if scale == 1.0 else f'{scale:.2f}x'}"
        else:
            caption = f"{self._bw.size[0]}×{self._bw.size[1]} @{'1:1' if scale == 1.0 else f'{scale:.2f}x'}"
        self.canvas.create_text(
            8,
            preview.size[1] + 4 if preview.size[1] < self.canvas.winfo_height() else preview.size[1] - 8,
            text=caption,
            fill="#c8c8b8",
            anchor="sw",
            font=("Consolas", 11),
        )


def main() -> int:
    app = A2i1App()
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
