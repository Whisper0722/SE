#pragma once

// =============================================================================
// db.hpp - 曲库与播放列表的 SQLite 持久层
//
// 职责：
//   * 维护 roots（扫描根目录）、tracks（曲目）、playlists / playlist_items
//   * 提供分页查询、FTS5 搜索、专辑聚合、播放列表增删改查
//
// 线程与锁：
//   * 所有 public 方法内部都用 mu_ 串行化；SQLite 连接本身也开了 FULLMUTEX。
//   * 扫描线程写入时按批次调用 upsertTracks，避免每首歌一个事务太慢。
//   * 事务中的 SQL 使用 execUnlocked()，因为此时已经持有 mu_，不能再递归加锁。
// =============================================================================

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "sqlite3.h"

namespace nf {

// 一条曲目在数据库中的完整快照，也是 RPC 返回给前端的对象。
struct TrackRecord {
  int64_t id = 0;            // SQLite 自增主键
  std::string path;          // 绝对路径（UTF-8）
  std::string title;         // 标题（TagLib 读取，读不到时用文件名）
  std::string artist;        // 艺人
  std::string album;         // 专辑
  std::string codec;         // 扩展名去掉点，如 "flac"
  int trackNo = 0;           // 音轨号
  int discNo = 0;            // 碟号（当前扫描未使用）
  int64_t durationMs = 0;    // 时长（毫秒）
  int sampleRate = 0;        // 采样率
  int channels = 0;          // 声道数
  int bitDepth = 0;          // 位深（TagLib 未提供时保持 0）
  int bitrate = 0;           // 码率 kbps
  int64_t mtime = 0;         // 文件修改时间（Unix 秒）
  int64_t size = 0;          // 文件大小（字节）
  int64_t addedAt = 0;       // 首次入库时间（毫秒）
};

// 分页结果：total 是满足条件的总数，tracks 是当前页数据。
struct TrackPage {
  int64_t total = 0;
  std::vector<TrackRecord> tracks;
};

// 专辑聚合视图（按 album 名称分组）。
struct AlbumInfo {
  std::string album;
  std::string artist;        // 该专辑下第一首的艺人
  int64_t trackCount = 0;
  int64_t durationMs = 0;    // 总时长
};

// 播放列表元信息（不含具体歌曲）。
struct PlaylistInfo {
  int64_t id = 0;
  std::string name;
  int64_t createdAt = 0;
  int64_t trackCount = 0;
};

class Db {
 public:
  // 打开/创建数据库，设置 WAL，并确保所有表和索引存在。
  // 失败时抛 std::runtime_error。
  explicit Db(const std::string& path);
  ~Db();

  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  bool ok() const;
  std::string lastError() const;

  // ---- 曲库根目录 ----
  void addRoot(const std::string& path);    // 已存在则忽略
  void removeRoot(const std::string& path); // 删除根目录，并清掉不再属于任何根目录的曲目
  std::vector<std::string> roots();
  // 删除不属于任何 roots 的曲目（孤儿），返回删除数量。
  // 用于修复旧版本 removeRoot 在中文路径下留下的残留。
  int64_t pruneOrphanTracks();

  // ---- 曲目 ----
  std::optional<TrackRecord> getTrack(int64_t id);
  // 读取 path 对应的 (mtime, size)，扫描时用来跳过未变化文件。
  std::optional<std::pair<int64_t, int64_t>> getTrackStamp(const std::string& path);
  // 批量 upsert（按 path 冲突更新），一个事务写完一批。
  void upsertTracks(const std::vector<TrackRecord>& tracks);
  // 扫描结束后删除该根目录下已经不存在的曲目；keep 是本轮看到的路径集合。
  void deleteTracksUnderRootNotIn(const std::string& root,
                                  const std::unordered_set<std::string>& keep);

  // 分页/搜索：
  //   query 非空时优先 FTS5（>=3 个字符），否则 LIKE；
  //   root 非空时只返回该目录前缀下的曲目；
  //   album 非空时只返回该专辑的曲目。
  TrackPage page(int64_t offset, int64_t limit, const std::string& query,
                 const std::string& root = "", const std::string& album = "");
  std::pair<int64_t, int64_t> stats();  // (曲目数, 专辑数)

  // 按专辑名聚合，用于专辑网格。
  std::vector<AlbumInfo> albums();

  // ---- 播放列表 ----
  std::vector<PlaylistInfo> playlists();
  std::optional<PlaylistInfo> playlist(int64_t id);
  int64_t createPlaylist(const std::string& name);
  void renamePlaylist(int64_t id, const std::string& name);
  void deletePlaylist(int64_t id);
  // 按前端给出的完整顺序重写 position，用于拖拽排序。
  void reorderPlaylist(int64_t id, const std::vector<int64_t>& trackIds);
  // 返回 false 表示播放列表或曲目不存在；重复添加会被 IGNORE。
  bool addTrackToPlaylist(int64_t playlistId, int64_t trackId);
  void removeTrackFromPlaylist(int64_t playlistId, int64_t trackId);
  // 按 position 顺序返回播放列表中的曲目。
  std::vector<TrackRecord> playlistTracks(int64_t playlistId);

 private:
  void exec(const std::string& sql);          // 加锁执行 SQL（用于建表/PRAGMA）
  void execUnlocked(const std::string& sql);   // 调用者已持锁时使用（事务）
  void createSchema();                        // CREATE TABLE/TRIGGER IF NOT EXISTS
  void setError(const std::string& message);
  int64_t pruneOrphanTracksLocked();           // 已持 mu_ 时使用

  sqlite3* db_ = nullptr;
  mutable std::mutex mu_;  // 串行化所有数据库访问
  std::string error_;      // 最近一次错误文本
};

}  // namespace nf
