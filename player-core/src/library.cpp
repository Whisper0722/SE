// =============================================================================
// library.cpp - 曲库扫描实现
//
// 扫描流程（scanRoot）：
//   1. 递归遍历根目录，按扩展名筛出音频文件
//   2. 先统计总数并发送一次 scan.progress
//   3. 逐文件比较 mtime+size；未变化就跳过（增量扫描）
//   4. 变化的文件用 TagLib 读元数据，攒够 64 条批量 upsert（一个事务）
//   5. 扫描结束后删除该根目录下已不存在的曲目
//
// 进度通知在扫描线程里直接调用 Notify，RpcServer 保证 stdout 写入线程安全。
// =============================================================================

#include "library.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include <audioproperties.h>
#include <fileref.h>
#include <tag.h>

#include "util.hpp"

namespace nf {
namespace {

// 支持扫描的扩展名。FFmpeg 实际能解更多格式；
// 这里只列常用音频容器，避免把视频/杂项文件当成歌曲。
const std::unordered_set<std::string> kAudioExtensions = {
    ".flac", ".mp3", ".m4a", ".aac", ".wav", ".ogg", ".oga",
    ".opus", ".wma",  ".aiff", ".aif", ".ape", ".wv", ".mpc", ".mka"};

// TagLib::String -> UTF-8 std::string（true 表示按 Unicode 输出）。
std::string tagString(const TagLib::String& s) { return s.to8Bit(true); }

}  // namespace

// 构造函数只保存依赖和回调，不启动线程。
Library::Library(Db& db, Notify notify) : db_(db), notify_(std::move(notify)) {}

// 析构时确保扫描线程已退出，避免访问已销毁的 Db。
Library::~Library() { shutdown(); }

// 请求停止扫描并等待线程结束。stop_ 是原子标志，扫描循环会定期检查。
void Library::shutdown() {
  stop_.store(true);
  std::lock_guard<std::mutex> lock(workerMutex_);
  if (worker_.joinable()) worker_.join();
}

// 安全地转发事件（notify_ 可能为空）。
void Library::notify(const std::string& method, const nlohmann::json& payload) const {
  if (notify_) notify_(method, payload);
}

// 添加根目录：
//   * weakly_canonical 统一路径（解析 ..、相对路径等）
//   * 校验确实是目录
//   * 写入 roots 表后立即异步扫描
void Library::addRoot(const std::string& path) {
  std::error_code ec;
  const auto p = std::filesystem::weakly_canonical(utf8ToPath(path), ec);
  if (ec || !std::filesystem::is_directory(p, ec)) {
    throw std::runtime_error("not a directory: " + path);
  }
  db_.addRoot(pathToUtf8(p));
  scanAsync();
}

// 移除根目录：Db 会删除该前缀下的所有曲目并清理播放列表，
// 然后通知前端刷新。
void Library::removeRoot(const std::string& path) {
  std::error_code ec;
  const auto p = std::filesystem::weakly_canonical(utf8ToPath(path), ec);
  const std::string root = ec ? path : pathToUtf8(p);
  db_.removeRoot(root);
  const auto [tracks, albums] = db_.stats();
  notify("library.changed", {{"tracks", tracks}, {"albums", albums}});
}

// 启动扫描线程。
// scanning_ 用 exchange(true) 做“最多一个扫描”的原子保护；
// 若已有线程对象，先 join 再创建新线程（旧线程通常已结束）。
void Library::scanAsync() {
  if (scanning_.exchange(true)) return;
  stop_.store(false);
  std::lock_guard<std::mutex> lock(workerMutex_);
  if (worker_.joinable()) worker_.join();
  worker_ = std::thread([this] { scanWorker(); });
}

// 扫描线程入口：依次扫描所有根目录，最后发 library.changed。
// Guard 用 RAII 保证异常退出时 scanning_ 也会复位。
void Library::scanWorker() {
  struct Guard {
    std::atomic<bool>& flag;
    ~Guard() { flag.store(false); }
  } guard{scanning_};

  try {
    for (const auto& root : db_.roots()) {
      if (stop_.load()) break;
      scanRoot(root);
    }
    const auto [tracks, albums] = db_.stats();
    notify("library.changed", {{"tracks", tracks}, {"albums", albums}});
  } catch (const std::exception& e) {
    logMsg(std::string("scan failed: ") + e.what());
    notify("scan.error", {{"message", e.what()}});
  }
}

// 扫描单个根目录。
// 采用两遍：先收集所有音频文件得到 total，再逐个读取元数据，
// 这样前端能显示准确的 "已扫描 / 总数"。
void Library::scanRoot(const std::string& root) {
  namespace fs = std::filesystem;
  std::vector<fs::path> files;
  std::error_code ec;

  // skip_permission_denied 避免遇到无权限目录时直接抛异常。
  fs::recursive_directory_iterator it(
      utf8ToPath(root), fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  while (it != end) {
    if (stop_.load()) return;
    std::error_code fileEc;
    if (it->is_regular_file(fileEc)) {
      if (kAudioExtensions.count(lowerExtension(it->path()))) files.push_back(it->path());
    }
    it.increment(ec);
    if (ec) {
      // 单个目录读取失败时清掉错误继续，其他目录仍可扫描。
      ec.clear();
      if (it == end) break;
    }
  }

  const auto total = static_cast<int64_t>(files.size());
  logMsg("scan root " + root + " (" + std::to_string(total) + " files)");
  notify("scan.progress",
         {{"root", root}, {"scanned", 0}, {"total", total}, {"indexed", 0}});

  // seen 记录本轮见到的路径，用于最后删除已不存在的曲目。
  std::unordered_set<std::string> seen;
  seen.reserve(files.size() * 2);

  // 批量写库，每 64 条提交一次，兼顾速度和内存。
  std::vector<TrackRecord> batch;
  batch.reserve(64);
  int64_t scanned = 0;
  int64_t indexed = 0;

  auto flush = [&]() {
    if (batch.empty()) return;
    db_.upsertTracks(batch);
    batch.clear();
  };

  for (const auto& file : files) {
    if (stop_.load()) break;
    const std::string path = pathToUtf8(file);
    if (path.empty()) continue;
    seen.insert(path);

    // 文件大小和修改时间用于增量判断。
    std::error_code sizeEc;
    std::error_code timeEc;
    const int64_t size = static_cast<int64_t>(fs::file_size(file, sizeEc));
    const auto stamp = db_.getTrackStamp(path);
    int64_t mtime = 0;
    if (const auto ft = fs::last_write_time(file, timeEc); !timeEc) {
      mtime = fileTimeToUnix(ft);
    }

    // 路径已存在且 mtime/size 都没变：跳过元数据解析，只计入进度。
    if (stamp && stamp->first == mtime && stamp->second == size) {
      ++scanned;
      continue;
    }

    TrackRecord track;
    track.path = path;
    track.codec = lowerExtension(file);
    if (!track.codec.empty() && track.codec.front() == '.') track.codec.erase(0, 1);
    track.mtime = mtime;
    track.size = size;
    track.addedAt = nowMs();

    // TagLib 读取标题/艺人/专辑/音轨号。
    // FileName(file.c_str()) 在 Windows 上使用宽字符路径，支持中文文件名。
    TagLib::FileRef ref(TagLib::FileName(file.c_str()), true,
                        TagLib::AudioProperties::Average);
    if (!ref.isNull() && ref.tag()) {
      const auto* tag = ref.tag();
      track.title = tagString(tag->title());
      track.artist = tagString(tag->artist());
      track.album = tagString(tag->album());
      track.trackNo = tag->track();
      track.discNo = 0;  // TagLib 通用 Tag 接口没有碟号，暂不读取
    }
    // 没有标题时退回文件名（不含扩展名），避免列表出现空白。
    if (track.title.empty()) track.title = pathToUtf8(file.stem());
    if (!ref.isNull() && ref.audioProperties()) {
      const auto* props = ref.audioProperties();
      track.durationMs = props->lengthInMilliseconds();
      track.sampleRate = props->sampleRate();
      track.channels = props->channels();
      track.bitrate = props->bitrate();
    }

    batch.push_back(std::move(track));
    ++indexed;
    ++scanned;

    if (batch.size() >= 64) flush();
    // 每 20 个文件报一次进度，避免通知过于频繁。
    if (scanned % 20 == 0) {
      notify("scan.progress",
             {{"root", root},
              {"scanned", scanned},
              {"total", total},
              {"indexed", indexed}});
    }
  }

  flush();
  // 清理该根目录下已删除/改名/移走的曲目索引。
  db_.deleteTracksUnderRootNotIn(root, seen);
  notify("scan.progress",
         {{"root", root}, {"scanned", scanned}, {"total", total}, {"indexed", indexed}});
}

}  // namespace nf
