#!/usr/bin/env bash
# 将 fonts.fontpack 烧进 font_data 分区。
# 用法：
#   ./flash_fontpack.sh [/dev/ttyACM0] [path/to/fonts.fontpack]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PORT="${1:-/dev/ttyACM0}"
PACK="${2:-$(cd "$(dirname "$0")" && pwd)/fonts.fontpack}"

if [[ ! -f "$PACK" ]]; then
  echo "fontpack not found: $PACK" >&2
  exit 1
fi

if ! command -v parttool.py >/dev/null 2>&1; then
  # ESP-IDF 环境
  if [[ -n "${IDF_PATH:-}" ]]; then
    PARTTOOL="$IDF_PATH/components/partition_table/parttool.py"
  else
    echo "parttool.py not in PATH; source export.sh first" >&2
    exit 1
  fi
else
  PARTTOOL=parttool.py
fi

echo "Writing $PACK -> partition font_data on $PORT"
python3 "$PARTTOOL" --port "$PORT" write_partition \
  --partition-name font_data \
  --input "$PACK"
echo "Done."
