#!/usr/bin/env bash
# MiSans-Mixed 生产 fontpack：汉字 + Noto Emoji 30@2 + Latin Modern Math 18/28/36@2。
# 不再单独维护无 emoji 的 fonts_misans_25_30.fontpack。
#
# 用法（在 tools/fontpack/ 下）：
#   ./build_misans_mixed.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

PY="${ROOT}/.venv/bin/python"
if [[ ! -x "$PY" ]]; then
  echo "venv missing; run: python3 -m venv .venv && .venv/bin/pip install -r requirements.txt" >&2
  exit 1
fi

EF="${ROOT}/../epdfont/MiSans-Light/misans_25_2.ef"
TTF="${ROOT}/../ttf-fonts/MiSans"
EMOJI_TTF="${ROOT}/../ttf-fonts/Noto_Emoji/static/NotoEmoji-Regular.ttf"
MATH_TTF="${ROOT}/../ttf-fonts/LatinModernMath/latinmodern-math.otf"
OUT_DIR="${ROOT}/fonts/MiSans-Mixed"
BASE_PACK="${OUT_DIR}/.build_base.fontpack"
EMOJI_PACK="${OUT_DIR}/.build_emoji.fontpack"
FINAL="${OUT_DIR}/fonts_misans_25_30.fontpack"

for f in "$EF" "${TTF}/MiSans-Light.ttf" "${TTF}/MiSans-Semibold.ttf" "$EMOJI_TTF" "$MATH_TTF"; do
  if [[ ! -f "$f" ]]; then
    echo "missing: $f" >&2
    exit 1
  fi
done

mkdir -p "$OUT_DIR"

echo "==> 1/3 MiSans-Mixed base (25@2 / 30@2 Light + 30@4 Semibold)"
"$PY" build_fontpack.py \
  --from-ef "$EF" \
  --variant-map "25:2=${TTF}/MiSans-Light.ttf,30:2=${TTF}/MiSans-Light.ttf,30:4=${TTF}/MiSans-Semibold.ttf" \
  -o "$BASE_PACK" --verify

echo "==> 2/3 merge Noto Emoji 30@2"
"$PY" merge_emoji_into_fontpack.py \
  --base "$BASE_PACK" \
  --emoji-ttf "$EMOJI_TTF" \
  -o "$EMOJI_PACK" --verify

echo "==> 3/3 merge Latin Modern Math 18/28/36@2"
"$PY" merge_math_into_fontpack.py \
  --base "$EMOJI_PACK" \
  --math-ttf "$MATH_TTF" \
  --chars-file "${ROOT}/charset_math.txt" \
  --sizes 18,28,36 \
  --bpp 2 \
  -o "$FINAL" --verify

rm -f "$BASE_PACK" "$EMOJI_PACK"
cp -f "$FINAL" "${ROOT}/fonts_misans_25_30.fontpack"
ln -sf "fonts_misans_25_30.fontpack" "${ROOT}/fonts.fontpack"
# 固件合并 / idf.py flash 默认读 use_font/font.fontpack
mkdir -p "${ROOT}/../../use_font"
cp -f "$FINAL" "${ROOT}/../../use_font/font.fontpack"

SZ=$(stat -c%s "$FINAL")
echo "DONE ${FINAL} ($(numfmt --to=iec-i --suffix=B "$SZ" 2>/dev/null || echo "${SZ} bytes"))"
echo "    -> fonts_misans_25_30.fontpack (flash default, +math)"
echo "    -> ../../use_font/font.fontpack (idf / merge_firmware)"
