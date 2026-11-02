#!/usr/bin/env bash
# =============================================================================
# build-core.sh - 用 MSYS2 MinGW64 工具链构建 C++ 核心
#
# 需要在 MSYS2 MINGW64 终端运行（cmake/ninja/gcc 都在 /mingw64/bin）。
# 额外参数会透传给 cmake --build，例如 --clean-first。
# =============================================================================
set -euo pipefail
# 以脚本所在目录为基准找到仓库根，避免依赖调用者的当前目录。
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/player-core"

# 配置：Ninja 生成器、Debug 构建（便于 gdb）。
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
# 构建；额外参数透传。
cmake --build build "$@"
