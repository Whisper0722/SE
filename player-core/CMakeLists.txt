# =============================================================================
# player-core - C++ 音频核心
#
# 依赖全部来自 MSYS2 MinGW64：
#   FFmpeg（解码/重采样）、TagLib（元数据）、SQLite3（曲库）、nlohmann/json（RPC）
# Windows 额外链接 ole32/uuid（WASAPI COM）。
#
# 用法（MSYS2 MINGW64 终端）：
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build
# =============================================================================

cmake_minimum_required(VERSION 3.20)
project(nightfall-core LANGUAGES CXX)

# C++20：使用 std::filesystem、std::jthread 之外的现代特性。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 没有指定构建类型时默认 Debug（便于用 gdb 调试）。
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# ---- 第三方依赖 ----
# FFmpeg 用 pkg-config 查找。四个库分别负责：
#   libavformat   容器/解复用
#   libavcodec    音频解码
#   libswresample 采样率/声道/样本格式转换
#   libavutil     通用工具（数学、声道布局等）
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat
  libavcodec
  libavutil
  libswresample
)
# TagLib 读取 FLAC/MP3/MP4 等文件的标题、艺人、专辑、时长。
pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET taglib)
# SQLite3 提供曲库/播放列表持久化；FTS5 用于搜索。
find_package(SQLite3 REQUIRED)
# 头文件库，用于 JSON-RPC 序列化。
find_package(nlohmann_json REQUIRED)

# ---- 可执行文件 ----
add_executable(player-core
  src/main.cpp          # 进程入口、参数解析、状态线程
  src/rpc.cpp           # stdio JSON-RPC 服务
  src/db.cpp            # SQLite 曲库与播放列表
  src/library.cpp       # 目录扫描与元数据读取
  src/playback.cpp      # 队列、解码线程、播放位置
  src/audio_output.cpp  # WASAPI / null 输出
)

target_include_directories(player-core PRIVATE src)
target_link_libraries(player-core PRIVATE
  PkgConfig::FFMPEG
  PkgConfig::TAGLIB
  SQLite3::SQLite3
  nlohmann_json::nlohmann_json
  ole32   # COM
  uuid    # WASAPI 接口 GUID
)

# MinGW 下把 libgcc/libstdc++ 静态链接，减少运行时分发 DLL 的数量。
# 注意：FFmpeg/TagLib/SQLite/libwinpthread 仍然需要动态 DLL，
# 由 scripts/bundle-runtime.sh 复制到 player-core/runtime。
if(MINGW)
  target_link_options(player-core PRIVATE -static-libgcc -static-libstdc++)
endif()

# 打开常见警告，便于发现类型/未使用变量等问题。
target_compile_options(player-core PRIVATE -Wall -Wextra -Wno-unused-parameter)
