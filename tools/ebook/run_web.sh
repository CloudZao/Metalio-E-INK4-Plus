#!/usr/bin/env bash
# 启动 ebook Web（转换 + 在线阅读）
# 用法:
#   ./run_web.sh
#   EBOOK_PORT=9000 ./run_web.sh
#   ./run_web.sh --kill   # 先结束占用默认端口的旧 web/app.py
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

PORT="${EBOOK_PORT:-${PORT:-8765}}"

if [[ "${1:-}" == "--kill" ]]; then
  if command -v fuser >/dev/null 2>&1; then
    fuser -k "${PORT}/tcp" 2>/dev/null || true
  else
    pids=$(ss -tlnp 2>/dev/null | awk -v p=":${PORT}" '$4 ~ p {print}' | sed -n 's/.*pid=\([0-9]*\).*/\1/p' | sort -u)
    for pid in $pids; do
      # 只杀本工具的 python web/app.py，避免误杀其它服务
      if tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q "web/app.py"; then
        kill "$pid" 2>/dev/null || true
      fi
    done
  fi
  sleep 0.4
  shift || true
fi

# 若 8765 上仍是旧的 web/app.py，自动结束再启
if command -v ss >/dev/null 2>&1; then
  old_pid=$(ss -tlnp 2>/dev/null | awk -v p=":${PORT}" '$4 ~ p {print}' | sed -n 's/.*pid=\([0-9]*\).*/\1/p' | head -1)
  if [[ -n "${old_pid:-}" ]]; then
    if tr '\0' ' ' < "/proc/$old_pid/cmdline" 2>/dev/null | grep -q "web/app.py"; then
      echo "结束旧实例 pid=$old_pid (port $PORT)"
      kill "$old_pid" 2>/dev/null || true
      sleep 0.4
    fi
  fi
fi

if [[ ! -x .venv/bin/python ]]; then
  python3 -m venv .venv
  .venv/bin/pip install -r requirements.txt
fi

export EBOOK_PORT="$PORT"
exec .venv/bin/python web/app.py
