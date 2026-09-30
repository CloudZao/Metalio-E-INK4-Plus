#!/usr/bin/env bash
# 构建 ebook-emulator WASM（需已安装并激活 Emscripten SDK）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
# 本仓布局：tools/ebook/emulator → ../../../main 、../web/static/wasm
ESP_MAIN="${ESP_MAIN:-$ROOT/../../../main}"
OUT_DIR="${OUT_DIR:-$ROOT/../web/static/wasm}"
BUILD="$ROOT/build-wasm"

if ! command -v emcc >/dev/null 2>&1; then
  echo "未找到 emcc。请先: source ~/emsdk/emsdk_env.sh" >&2
  exit 1
fi

mkdir -p "$BUILD" "$OUT_DIR"

WASM_CONF="$BUILD/lv_conf.h"
cp "$ROOT/lv_conf.h" "$WASM_CONF"
sed -i 's/LV_USE_VECTOR_GRAPHIC  1/LV_USE_VECTOR_GRAPHIC  0/' "$WASM_CONF"
sed -i 's/LV_USE_THORVG_INTERNAL 1/LV_USE_THORVG_INTERNAL 0/' "$WASM_CONF"
sed -i 's/LV_USE_LOTTIE     1/LV_USE_LOTTIE     0/' "$WASM_CONF"
sed -i 's/LV_USE_ASSERT_MEM_INTEGRITY 1/LV_USE_ASSERT_MEM_INTEGRITY 0/' "$WASM_CONF"
sed -i 's/LV_USE_ASSERT_OBJ           1/LV_USE_ASSERT_OBJ           0/' "$WASM_CONF"
sed -i 's/LV_USE_ASSERT_STYLE         1/LV_USE_ASSERT_STYLE         0/' "$WASM_CONF"
sed -i 's/#define LV_USE_LOG 1/#define LV_USE_LOG 0/' "$WASM_CONF"

LVGL="$ROOT/lvgl"
INC=(
  -I"$BUILD"
  -I"$ROOT"
  -I"$ROOT/src/esp_compat"
  -I"$ROOT/src/font"
  -I"$ROOT/src/emulator"
  -I"$ESP_MAIN/reader"
  -I"$LVGL"
)

DEFS=(
  -DEBOOK_EMULATOR=1
  -DLV_CONF_INCLUDE_SIMPLE
  -DLV_LVGL_H_INCLUDE_SIMPLE
)

CXXSRC=(
  "$ROOT/src/emulator/book_reader_emulator.cpp"
  "$ROOT/src/emulator/image_util_emulator.cc"
  "$ROOT/src/font/epdfont_emulator.cc"
  "$ESP_MAIN/display/font/epdfont.cc"
  "$ESP_MAIN/reader/book_session.cc"
  "$ESP_MAIN/reader/ebook_document.cc"
  "$ESP_MAIN/reader/epub_document.cc"
  "$ESP_MAIN/reader/html_content.cc"
  "$ESP_MAIN/reader/text_encoding.cc"
  "$ESP_MAIN/reader/txt_chapter.cc"
  "$ESP_MAIN/reader/zip_reader.cc"
)

CSRC=(
  "$ROOT/src/emulator/emulator_wasm.c"
)

OBJS=()
for f in "${CSRC[@]}"; do
  o="$BUILD/$(basename "${f%.*}").o"
  emcc -O2 "${DEFS[@]}" "${INC[@]}" -c "$f" -o "$o"
  OBJS+=("$o")
done
for f in "${CXXSRC[@]}"; do
  o="$BUILD/$(basename "${f%.*}").o"
  em++ -O2 -std=c++17 "${DEFS[@]}" "${INC[@]}" -c "$f" -o "$o"
  OBJS+=("$o")
done

mapfile -t LVGL_C < <(find "$LVGL/src" -name '*.c' \
  ! -path '*/examples/*' ! -path '*/demos/*' ! -path '*/tests/*' \
  ! -path '*/drivers/*' ! -path '*/libs/thorvg/*')
for f in "${LVGL_C[@]}"; do
  rel="${f#$LVGL/}"
  o="$BUILD/lvgl/${rel%.c}.o"
  mkdir -p "$(dirname "$o")"
  emcc -O2 "${DEFS[@]}" "${INC[@]}" -c "$f" -o "$o"
  OBJS+=("$o")
done

LDFLAGS=(
  -O2
  -s WASM=1
  -s USE_ZLIB=1
  -s ALLOW_MEMORY_GROWTH=0
  -s INITIAL_MEMORY=134217728
  -s EXPORTED_FUNCTIONS='["_emulator_init","_emulator_tick","_emulator_refresh","_emulator_set_fontpack","_emulator_set_epdfont","_emulator_open_ebook_path","_emulator_open_ebook_bytes","_emulator_next_page","_emulator_prev_page","_emulator_go_toc","_emulator_toc_count","_emulator_toc_title","_emulator_current_toc_index","_emulator_last_error","_malloc","_free"]'
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","FS","HEAPU8","HEAPU16"]'
  -o "$OUT_DIR/ebook_emulator.js"
)

em++ "${LDFLAGS[@]}" "${OBJS[@]}"
echo "WASM 已输出到 $OUT_DIR"
