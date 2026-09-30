#!/usr/bin/env bash
# 编译并合成 metalio-e-ink4-plus 完整固件：
#   以 build/flash_args 为准（CMake 已把 use_font/font.fontpack 挂到 font_data）
#   若旧构建未含 fontpack，则按分区表 font_data 偏移补上
# 输出：firmware/metalio-e-ink4-plus-{PROJECT_VER}.bin + 根目录 metalio-e-ink4-plus.bin
#       + daily-builds/metalio-e-ink4-YYYYMMDDHHMMSS.bin（本地归档，不提交 git）
#
# 用法:
#   ./merge_firmware.sh           # 编译+合并（默认不上传）
#   ./merge_firmware.sh -h
#   IDF_PATH=~/esp/v5.5.4 ./merge_firmware.sh
#
# 上传预留：日后可加 -u/--upload，当前未实现。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

FIRMWARE_DIR="firmware"
DAILY_BUILD_DIR="daily-builds"
PRODUCT_NAME="metalio-e-ink4"
FONT_PACK="use_font/font.fontpack"
FONT_PART_NAME="font_data"

usage() {
  cat <<'EOF'
用法: ./merge_firmware.sh [选项]

  默认编译并合并完整固件（含 font_data 分区的 font.fontpack），不上传。

选项:
  -h, --help     显示帮助

说明:
  优先使用 build/flash_args（含 CMake 挂载的 fontpack）；若缺失再按分区表
  Name=font_data 的 Offset 补写 use_font/font.fontpack，避免地址重叠。
  上传功能尚未接入，后续再加 -u/--upload。
EOF
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -h|--help)
        usage
        exit 0
        ;;
      -u|--upload)
        echo "错误: 上传尚未实现，请先不加 -u 仅做本地合并" >&2
        exit 1
        ;;
      *)
        echo "未知参数: $1" >&2
        usage >&2
        exit 1
        ;;
    esac
  done
}

# ---------- IDF 环境 ----------
setup_idf() {
  if command -v idf.py >/dev/null 2>&1 && [[ -n "${IDF_PATH:-}" ]]; then
    echo "[idf] 已激活: IDF_PATH=${IDF_PATH}"
    return 0
  fi

  if [[ -z "${IDF_PATH:-}" ]] || [[ ! -f "${IDF_PATH}/export.sh" ]]; then
    local candidates=(
      "${HOME}/esp/v5.5.4"
      "${HOME}/esp/esp-idf"
      "${HOME}/esp-idf"
      "${HOME}/.espressif/v5.5.4/esp-idf"
      "/opt/esp/idf"
    )
    local c
    for c in "${candidates[@]}"; do
      if [[ -f "${c}/export.sh" ]]; then
        IDF_PATH="$c"
        break
      fi
    done
  fi

  if [[ -z "${IDF_PATH:-}" ]] || [[ ! -f "${IDF_PATH}/export.sh" ]]; then
    echo "错误: 未找到 ESP-IDF export.sh。请设置 IDF_PATH 后重试，例如:" >&2
    echo "  export IDF_PATH=~/esp/v5.5.4 && ./merge_firmware.sh" >&2
    exit 1
  fi

  echo "[idf] source ${IDF_PATH}/export.sh"
  # shellcheck disable=SC1091
  source "${IDF_PATH}/export.sh"

  if ! command -v idf.py >/dev/null 2>&1; then
    echo "错误: source 后仍找不到 idf.py" >&2
    exit 1
  fi
}

# ---------- 读版本 ----------
get_project_ver() {
  local ver
  ver="$(sed -n 's/^set(PROJECT_VER "\([^"]*\)").*/\1/p' CMakeLists.txt | head -1)"
  if [[ -z "$ver" ]]; then
    echo "错误: 无法从 CMakeLists.txt 解析 PROJECT_VER" >&2
    exit 1
  fi
  echo "$ver"
}

# ---------- 从已构建分区表解析 font_data Offset/Size ----------
# 输出到全局：FONT_ADDR FONT_SIZE（十进制字节；ADDR 打印用十六进制）
resolve_font_partition() {
  local part_bin="build/partition_table/partition-table.bin"
  if [[ ! -f "$part_bin" ]]; then
    echo "错误: 缺少 ${part_bin}，请先完成编译" >&2
    exit 1
  fi

  local parsed
  parsed="$(
    python3 - "$part_bin" "$FONT_PART_NAME" <<'PY'
import struct, sys
path, want = sys.argv[1], sys.argv[2]
data = open(path, "rb").read()
MAGIC = 0x50AA
off = 0
while off + 32 <= len(data):
    magic, _type, _subtype, offset, size = struct.unpack_from("<HBBII", data, off)
    if magic != MAGIC:
        break
    label = data[off + 12 : off + 28].split(b"\x00", 1)[0].decode("ascii", "replace")
    if label == want:
        print(f"{offset} {size}")
        sys.exit(0)
    off += 32
sys.stderr.write(f"错误: 分区表中未找到 {want}\n")
sys.exit(1)
PY
  )"

  FONT_ADDR_DEC="$(awk '{print $1}' <<<"$parsed")"
  FONT_SIZE="$(awk '{print $2}' <<<"$parsed")"
  FONT_ADDR="$(printf '0x%x' "$FONT_ADDR_DEC")"

  echo "[partition] ${FONT_PART_NAME} offset=${FONT_ADDR} size=$(printf '0x%x' "$FONT_SIZE") (${FONT_SIZE} bytes)"
}

# ---------- 收集 merge 参数 ----------
# flash_args 路径相对 build/；可能是 bootloader/... 或 ../use_font/...
resolve_flash_path() {
  local file="$1"
  if [[ "$file" == /* ]]; then
    echo "$file"
  else
    echo "build/${file}"
  fi
}

# 十六进制地址规范化为小写 0x...（便于与 FONT_ADDR 比较）
norm_hex_addr() {
  printf '0x%x' "$(( $1 ))"
}

collect_merge_args() {
  MERGE_ARGS=()

  local flash_args="build/flash_args"
  if [[ ! -f "$flash_args" ]]; then
    echo "错误: 缺少 ${flash_args}，请先完成编译" >&2
    exit 1
  fi

  local font_already=0
  while read -r addr file; do
    [[ -z "${addr:-}" ]] && continue
    [[ "$addr" == --* ]] && continue
    local path
    path="$(resolve_flash_path "$file")"
    if [[ ! -f "$path" ]]; then
      echo "错误: 缺少固件文件 ${path}" >&2
      exit 1
    fi
    # 归一化路径，避免 merge 时出现 build/../use_font/...
    path="$(cd "$(dirname "$path")" && pwd)/$(basename "$path")"
    local addr_n
    addr_n="$(norm_hex_addr "$addr")"
    echo "[build] ${addr_n} ${path}"
    MERGE_ARGS+=("$addr_n" "$path")
  done < <(grep -E '^[0-9a-fxA-FX]+[[:space:]]' "$flash_args" || true)

  resolve_font_partition

  # CMake esptool_py_flash_to_partition 已写入 flash_args 时勿再追加，否则 overlap
  local i
  for ((i = 0; i < ${#MERGE_ARGS[@]}; i += 2)); do
    if [[ "${MERGE_ARGS[$i]}" == "$FONT_ADDR" ]]; then
      font_already=1
      break
    fi
  done

  if [[ ! -f "$FONT_PACK" ]]; then
    echo "错误: 缺少字体包 ${FONT_PACK}" >&2
    exit 1
  fi
  local font_bytes
  font_bytes="$(wc -c < "$FONT_PACK" | tr -d ' ')"
  if (( font_bytes > FONT_SIZE )); then
    echo "错误: ${FONT_PACK} (${font_bytes} bytes) 超过 ${FONT_PART_NAME} 分区 (${FONT_SIZE} bytes)" >&2
    exit 1
  fi

  if (( font_already )); then
    echo "[font] 已在 flash_args: ${FONT_ADDR} (${font_bytes} bytes)，跳过重复追加"
  else
    echo "[font] flash_args 未含 fontpack，补上 ${FONT_ADDR} ${FONT_PACK} (${font_bytes} bytes)"
    MERGE_ARGS+=("$FONT_ADDR" "$FONT_PACK")
  fi

  if [[ ${#MERGE_ARGS[@]} -eq 0 ]]; then
    echo "错误: 没有可合并的固件段" >&2
    exit 1
  fi
}

# ---------- 主流程 ----------
main() {
  setup_idf

  local ver
  ver="$(get_project_ver)"
  mkdir -p "$FIRMWARE_DIR"
  local out="${FIRMWARE_DIR}/${PRODUCT_NAME}-${ver}.bin"
  echo "[ver] PROJECT_VER=${ver} -> ${out}"

  echo "[build] idf.py build ..."
  idf.py build

  collect_merge_args

  # 优先与 build/flasher_args.json / flash_args 一致；可用环境变量覆盖
  local chip="${ESPTOOL_CHIP:-esp32s3}"
  local flash_mode="${FLASH_MODE:-dio}"
  local flash_freq="${FLASH_FREQ:-80m}"
  local flash_size="${FLASH_SIZE:-16MB}"

  # 若 flash_args 首行带 --flash_*，覆盖默认
  if [[ -f build/flash_args ]]; then
    local line
    line="$(head -1 build/flash_args || true)"
    if [[ "$line" == --flash_* ]]; then
      # shellcheck disable=SC2086
      set -- $line
      while [[ $# -gt 0 ]]; do
        case "$1" in
          --flash_mode) flash_mode="$2"; shift 2 ;;
          --flash_freq) flash_freq="$2"; shift 2 ;;
          --flash_size) flash_size="$2"; shift 2 ;;
          *) shift ;;
        esac
      done
    fi
  fi

  echo "[merge] esptool.py --chip ${chip} merge_bin -> ${out}"
  echo "[merge] flash_mode=${flash_mode} flash_freq=${flash_freq} flash_size=${flash_size}"
  esptool.py --chip "$chip" merge_bin \
    -o "$out" \
    --flash_mode "$flash_mode" \
    --flash_freq "$flash_freq" \
    --flash_size "$flash_size" \
    "${MERGE_ARGS[@]}"

  local latest="${PRODUCT_NAME}.bin"
  cp -f "$out" "$latest"

  mkdir -p "$DAILY_BUILD_DIR"
  local daily_name="${PRODUCT_NAME}-$(date '+%Y%m%d%H%M%S').bin"
  local daily_out="${DAILY_BUILD_DIR}/${daily_name}"
  cp -f "$out" "$daily_out"

  local size
  size="$(wc -c < "$out" | tr -d ' ')"
  echo "[done] ${SCRIPT_DIR}/${out} (${size} bytes)"
  echo "[done] ${SCRIPT_DIR}/${latest}"
  echo "[done] ${SCRIPT_DIR}/${daily_out}"
  echo "[upload] 已跳过（上传功能尚未接入）"
}

parse_args "$@"
main
