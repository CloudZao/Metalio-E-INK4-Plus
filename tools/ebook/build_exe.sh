#!/usr/bin/env bash
# 在 WSL 下调用 Windows Python 打包 ebook-web.exe
# 用法: ./build_exe.sh
#
# 说明: 直接从 WSL UNC 路径跑 cmd 会失败，因此同步到
#       C:\Users\<you>\ebook-web-build 再构建，完成后拷回 dist/
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

if ! command -v cmd.exe >/dev/null 2>&1; then
  echo "未找到 cmd.exe（需要 WSL + Windows）。也可在资源管理器中双击 build_exe.bat。"
  exit 1
fi

WIN_USER=$(cmd.exe /c "echo %USERNAME%" 2>/dev/null | tr -d '\r')
WIN_BUILD="/mnt/c/Users/${WIN_USER}/ebook-web-build"
mkdir -p "$WIN_BUILD"

# 打包资源：.ef 字体 + fontpack（打进 web/static，PyInstaller 一并收集）
EF_SRC="$ROOT/../epdfont/MiSans-Light"
FP_SRC="$ROOT/../fontpack/fonts/MiSans-Mixed/fonts_misans_25_30.fontpack"
EF_DST="$ROOT/web/static/fonts/ef"
FP_DST="$ROOT/web/static/fonts"
mkdir -p "$EF_DST" "$FP_DST"
if [[ -d "$EF_SRC" ]]; then
  cp -f "$EF_SRC"/*.ef "$EF_DST/"
  echo "已同步 $(find "$EF_DST" -maxdepth 1 -name '*.ef' | wc -l) 个 .ef 字体 → web/static/fonts/ef/"
else
  echo "WARN: 未找到 $EF_SRC，exe 内墨水屏 .ef 字体可能缺失"
fi
if [[ -f "$FP_SRC" ]]; then
  cp -f "$FP_SRC" "$FP_DST/"
  echo "已同步 fontpack → web/static/fonts/"
else
  echo "WARN: 未找到 $FP_SRC，exe 内 UI fontpack 可能缺失"
fi

echo "同步源码 → $WIN_BUILD"
rsync -a --delete \
  --exclude '.venv/' \
  --exclude '.venv-win/' \
  --exclude 'data/' \
  --exclude 'dist/' \
  --exclude 'build/' \
  --exclude 'book/' \
  --exclude 'test-book/' \
  --exclude '__pycache__/' \
  --exclude '*.pyc' \
  "$ROOT/" "$WIN_BUILD/"

# 保证 bat 为 CRLF
python3 - <<PY
from pathlib import Path
p = Path("$WIN_BUILD") / "build_exe.bat"
text = p.read_text(encoding="utf-8").replace("\r\n", "\n").replace("\n", "\r\n")
p.write_bytes(text.encode("utf-8"))
PY

echo "开始 Windows 打包..."
cmd.exe /c "cd /d C:\\Users\\${WIN_USER}\\ebook-web-build && build_exe.bat"

mkdir -p "$ROOT/dist"
cp -f "$WIN_BUILD/dist/ebook-web.exe" "$ROOT/dist/ebook-web.exe"
echo "完成: $ROOT/dist/ebook-web.exe"
ls -lah "$ROOT/dist/ebook-web.exe"
