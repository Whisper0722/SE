#!/usr/bin/env bash
# Copy the full DLL closure of player-core.exe into player-core/runtime/.
# Run inside a MSYS2 MINGW64 terminal (needs ldd from the MinGW toolchain).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE="$ROOT/player-core/build/player-core.exe"
OUT="$ROOT/player-core/runtime"

if [ ! -f "$EXE" ]; then
  echo "missing core executable: $EXE" >&2
  exit 1
fi

mkdir -p "$OUT"

# 依赖可能还有依赖（FFmpeg -> libx264 -> zlib ...），所以用工作队列做传递闭包：
#   seen 按 DLL 文件名去重；queue 存放还需要继续解析的 DLL。
declare -A seen=()
queue=("$EXE")
copied=0

while [ "${#queue[@]}" -gt 0 ]; do
  item="${queue[0]}"
  queue=("${queue[@]:1}")

  # ldd 输出形如 "libfoo.dll => /mingw64/bin/libfoo.dll (0x...)"。
  # 只关心解析到 /mingw64/bin 的 DLL；KERNEL32 等系统 DLL 由 Windows 提供。
  while IFS= read -r dll; do
    [ -z "$dll" ] && continue
    name="$(basename "$dll")"
    if [ -n "${seen[$name]:-}" ]; then
      continue
    fi
    seen["$name"]=1
    # 复制并加入队列，继续解析它自己的依赖。
    cp -f "$dll" "$OUT/$name"
    copied=$((copied + 1))
    queue+=("$dll")
  done < <(
    ldd "$item" 2>/dev/null |
      awk '/=> \/mingw64\/bin\// {print $3} /^\/mingw64\/bin\// {print $1}' |
      sort -u
  )
done

# 最后再检查一遍直接依赖里是否有 not found（只提示，不中断）。
missing="$(ldd "$EXE" | awk '/not found/ {print $1}' | sort -u)"
if [ -n "$missing" ]; then
  echo "warning: unresolved dependencies:" >&2
  echo "$missing" >&2
fi

echo "copied $copied DLLs to $OUT"
