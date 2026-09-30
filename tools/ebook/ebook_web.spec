# -*- mode: python ; coding: utf-8 -*-
# PyInstaller spec：打包 ebook Web 为单文件 exe
# 构建: pyinstaller ebook_web.spec
# 产物: dist/ebook-web.exe

from pathlib import Path

from PyInstaller.utils.hooks import collect_all

block_cipher = None
ROOT = Path(SPECPATH).resolve()

datas = [
    (str(ROOT / "web" / "templates"), "templates"),
    (str(ROOT / "web" / "static"), "static"),
    (str(ROOT / "epdbook"), "epdbook"),
]
binaries = []
hiddenimports = [
    "epdbook",
    "epdbook.convert_api",
    "epdbook.cover_ops",
    "epdbook.meta_extra",
    "epdbook.format",
    "epdbook.reader",
    "epdbook.writer",
    "epdbook.chapter_detect",
    "epdbook.bookmarks",
    "epdbook.image_prep",
    "epdbook.preview",
    "epdbook.extractors",
    "epdbook.extractors.txt",
    "epdbook.extractors.epub",
    "epdbook.extractors.pdf",
    "epdbook.extractors.mobi",
    "flask",
    "jinja2",
    "werkzeug",
    "PIL",
    "fitz",
    "ebooklib",
    "ebooklib.epub",
    "mobi",
]

for pkg in ("fitz", "pymupdf", "PIL", "ebooklib", "mobi"):
    try:
        pkg_datas, pkg_binaries, pkg_hidden = collect_all(pkg)
        datas += pkg_datas
        binaries += pkg_binaries
        hiddenimports += pkg_hidden
    except Exception:
        pass

_seen = set()
_uniq_datas = []
for src, dest in datas:
    key = (str(src), str(dest))
    if key not in _seen:
        _seen.add(key)
        _uniq_datas.append((src, dest))
datas = _uniq_datas

a = Analysis(
    [str(ROOT / "web" / "app.py")],
    pathex=[str(ROOT)],
    binaries=binaries,
    datas=datas,
    hiddenimports=sorted(set(hiddenimports)),
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.zipfiles,
    a.datas,
    [],
    name="ebook-web",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=True,  # 保留控制台：显示地址；关闭窗口即停服
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
