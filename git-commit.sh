#!/usr/bin/env bash
set -euo pipefail

# 一键提交并推送到远程仓库
# 用法:
#   ./git-commit.sh           # 默认提交信息: daily commit
#   ./git-commit.sh "自定义提交信息"

export GIT_PAGER=cat

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

COMMIT_MSG="${1:-daily commit}"

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "错误: 当前目录不是 git 仓库"
  exit 1
fi

HAS_CHANGES=false
if ! git diff --quiet || ! git diff --cached --quiet || [ -n "$(git ls-files --others --exclude-standard)" ]; then
  HAS_CHANGES=true
fi

if [ "$HAS_CHANGES" = true ]; then
  echo "==> 暂存所有变更..."
  git add -A

  echo "==> 提交: $COMMIT_MSG"
  git commit -m "$COMMIT_MSG"
else
  echo "==> 没有新的本地变更，跳过提交"
fi

UPSTREAM="$(git rev-parse --abbrev-ref --symbolic-full-name @{u} 2>/dev/null || true)"
if [ -z "$UPSTREAM" ]; then
  echo "错误: 当前分支未设置上游远程分支，请先执行: git push -u origin HEAD"
  exit 1
fi

LOCAL="$(git rev-parse HEAD)"
REMOTE="$(git rev-parse "$UPSTREAM" 2>/dev/null || echo "")"

if [ "$LOCAL" = "$REMOTE" ] && [ "$HAS_CHANGES" = false ]; then
  echo "没有可提交或推送的内容"
  exit 0
fi

echo "==> 推送到远程: $UPSTREAM"
git push

echo "==> 完成"
git --no-pager log -1 --oneline
git status -sb
