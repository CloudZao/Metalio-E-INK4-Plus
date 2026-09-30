#!/usr/bin/env bash
# 将 assets/epd_font24.bin 烧进 font_data 分区。
# 用法：./tools/flash_epd_font24.sh [PORT]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${1:-}"
BIN="${ROOT}/assets/epd_font24.bin"

if [[ ! -f "$BIN" ]]; then
  echo "missing $BIN — run: python tools/gen_epd_font24_bin.py" >&2
  exit 1
fi

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "source ESP-IDF export.sh first" >&2
  exit 1
fi

PARTTOOL="$IDF_PATH/components/partition_table/parttool.py"
ARGS=(write_partition --partition-name font_data --input "$BIN")
if [[ -n "$PORT" ]]; then
  ARGS=(--port "$PORT" "${ARGS[@]}")
fi
echo "Writing $BIN -> font_data"
python "$PARTTOOL" "${ARGS[@]}"
echo "Done."
