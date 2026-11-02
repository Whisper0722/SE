#pragma once

// =============================================================================
// util.hpp - 核心进程的公共工具
//
// 这里只放“与业务无关、但到处都要用”的小工具：
//   * 文件系统路径与 UTF-8 字符串互转（Windows 下 path 内部是 wchar_t）
//   * 文件扩展名、时间戳
//   * 线程安全的 stderr 日志
//
// 重要约定：stdout 专门用于 JSON-RPC 帧，日志一律写 stderr。
// =============================================================================

#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <sys/stat.h>
#endif

namespace nf {

// 把 std::filesystem::path 转成 UTF-8 std::string。
// C++20 的 u8string() 返回 char8_t 字符串，这里逐字节转成普通 char，
// 便于存进 SQLite、写进 JSON、传给 FFmpeg（它们都按 UTF-8 处理）。
inline std::string pathToUtf8(const std::filesystem::path& p) {
  auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
}

// pathToUtf8 的逆操作：把 UTF-8 字符串还原为文件系统路径。
// Windows 上 path 会正确保存为宽字符路径，避免中文/空格文件名丢失。
inline std::filesystem::path utf8ToPath(const std::string& s) {
  std::u8string u8;
  u8.reserve(s.size());
  for (unsigned char c : s) u8.push_back(static_cast<char8_t>(c));
  return std::filesystem::path(u8);
}

// 返回小写扩展名，包含开头的点，例如 ".flac"。
// 用于扫描时判断文件类型；大小写不敏感。
inline std::string lowerExtension(const std::filesystem::path& p) {
  std::string ext = pathToUtf8(p.extension());
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

// 当前 Unix 毫秒时间戳，用于数据库中的 added_at、文件变化检测等。
inline int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 把 filesystem 的“文件修改时间”换算成 Unix 秒。
// filesystem::file_time_type 的时钟原点与 system_clock 不同，
// 因此先转到 system_clock，再取秒数。扫描时用 mtime+size 判断文件是否变化。
inline int64_t fileTimeToUnix(const std::filesystem::file_time_type& ft) {
  using namespace std::chrono;
  const auto sys = time_point_cast<system_clock::duration>(
      ft - std::filesystem::file_time_type::clock::now() + system_clock::now());
  return duration_cast<seconds>(sys.time_since_epoch()).count();
}

// 线程安全地往 stderr 打一行带时分的日志。
// 注意：绝不能用 stdout，否则会破坏 JSON-RPC 的 Content-Length 帧。
inline void logMsg(const std::string& msg) {
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char buf[32] = {0};
  std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
  std::cerr << "[" << buf << "] " << msg << '\n' << std::flush;
}

}  // namespace nf
