#!/bin/bash

# 自动构建并运行 lvglsim 的脚本
# 使用方法：赋予执行权限后直接运行 ./build_and_run.sh

set -e  # 遇到错误立即退出

echo "🔨 开始构建项目..."
cmake --build build -j$(nproc)

if [ $? -eq 0 ]; then
    echo "✅ 构建成功！"
    echo "🚀 正在运行 lvglsim..."
    ./build/bin/lvglsim
else
    echo "❌ 构建失败，请检查错误信息。"
    exit 1
fi
