// =============================================================================
// db.cpp - SQLite 曲库/播放列表实现
//
// 表：roots / tracks / tracks_fts / playlists / playlist_items。
// 所有 SQL 都使用 sqlite3_bind_* 参数绑定，避免注入。
// 读路径（RPC 查询）与写路径（扫描、播放列表）共用同一个连接，
// 因此 public 方法统一用 mu_ 串行化；WAL 模式下扫描写入不会阻塞太多读。
// =============================================================================

#include "db.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include "util.hpp"

namespace nf {
namespace {

// 所有曲目查询都按这个列顺序 SELECT，readTrack() 按同样顺序读取。
// 修改列顺序时必须同步修改 readTrack() 的下标。
constexpr const char* kTrackColumns =
    "t.id, t.path, t.title, t.artist, t.album, t.codec, t.track_no, t.disc_no, "
    "t.duration_ms, t.sample_rate, t.channels, t.bit_depth, t.bitrate, t.mtime, "
    "t.size, t.added_at";

// 从 SQLite 结果集的一行构造 TrackRecord。
// 列下标与 kTrackColumns 一一对应；空值统一转成空字符串。
TrackRecord readTrack(sqlite3_stmt* stmt) {
  TrackRecord t;
  t.id = sqlite3_column_int64(stmt, 0);
  auto text = [stmt](int i) -> std::string {
    const unsigned char* p = sqlite3_column_text(stmt, i);
    return p ? reinterpret_cast<const char*>(p) : "";
  };
  t.path = text(1);
  t.title = text(2);
  t.artist = text(3);
  t.album = text(4);
  t.codec = text(5);
  t.trackNo = sqlite3_column_int(stmt, 6);
  t.discNo = sqlite3_column_int(stmt, 7);
  t.durationMs = sqlite3_column_int64(stmt, 8);
  t.sampleRate = sqlite3_column_int(stmt, 9);
  t.channels = sqlite3_column_int(stmt, 10);
  t.bitDepth = sqlite3_column_int(stmt, 11);
  t.bitrate = sqlite3_column_int(stmt, 12);
  t.mtime = sqlite3_column_int64(stmt, 13);
  t.size = sqlite3_column_int64(stmt, 14);
  t.addedAt = sqlite3_column_int64(stmt, 15);
  return t;
}

// 统计 UTF-8 字符数（不是字节数）。
// FTS5 trigram 分词器至少需要 3 个字符才有效，1~2 个中文字符的查询退回 LIKE，
// 否则会出现“明明有结果却搜不到”的问题。
std::size_t utf8Codepoints(const std::string& s) {
  std::size_t count = 0;
  for (unsigned char c : s) {
    if ((c & 0xC0) != 0x80) ++count;
  }
  return count;
}

// 去掉 query 首尾空白；全空白视为空查询。
std::string trim(const std::string& s) {
  const auto begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

// 把用户输入包成 FTS5 短语：内部的双引号需要双写。
// 这样用户输入里的 AND/OR/NEAR 等不会被当成 FTS 语法。
std::string escapeFtsPhrase(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    if (c == '"') out.push_back('"');
    out.push_back(c);
  }
  return out;
}

// 判断 path 是否在 root 目录下。
//   * 比较前缀时忽略 ASCII 大小写，并把 / 与 \ 视为等同（Windows 路径）
//   * root 后必须是路径分隔符，避免 "I:\Music" 误匹配 "I:\Music2"
//   * root 自身以分隔符结尾（如 "D:\"）时，前缀匹配即可
// 注意：这里只比较字节，中文 UTF-8 字节保持一致即可，不做 Unicode 大小写折叠。
bool isUnderRoot(const std::string& path, const std::string& root) {
  if (root.empty() || path.size() < root.size()) return false;

  auto normalize = [](char c) -> char {
    if (c == '/') return '\\';
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
    return c;
  };

  for (std::size_t i = 0; i < root.size(); ++i) {
    if (normalize(path[i]) != normalize(root[i])) return false;
  }
  if (path.size() == root.size()) return true;

  // "D:\" 这类根本身带分隔符，前缀匹配后无需再检查下一个字符。
  const char last = root.back();
  if (last == '\\' || last == '/') return true;

  const char next = path[root.size()];
  return next == '\\' || next == '/';
}

}  // namespace

// 打开数据库并初始化。
// FULLMUTEX 让 SQLite 自身也线程安全；我们额外用 mu_ 串行化业务方法。
// WAL + synchronous=NORMAL 在扫描大量文件时明显更快。
Db::Db(const std::string& path) {
  const int rc = sqlite3_open_v2(
      path.c_str(), &db_,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
  if (rc != SQLITE_OK) {
    setError(db_ ? sqlite3_errmsg(db_) : "failed to open database");
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
    throw std::runtime_error("db open failed: " + error_);
  }
  sqlite3_extended_result_codes(db_, 1);
  exec("PRAGMA journal_mode=WAL;");
  exec("PRAGMA synchronous=NORMAL;");
  exec("PRAGMA foreign_keys=ON;");
  createSchema();
}

// 关闭连接。调用方应确保没有其他线程还在使用 Db。
Db::~Db() {
  if (db_) sqlite3_close(db_);
}

// 连接是否成功打开。
bool Db::ok() const { return db_ != nullptr; }

// 最近一次错误文本（加锁读取）。
std::string Db::lastError() const {
  std::lock_guard<std::mutex> lock(mu_);
  return error_;
}

// 记录错误并打到 stderr。调用者通常已持有 mu_，所以这里不再加锁。
void Db::setError(const std::string& message) {
  error_ = message;
  logMsg("db error: " + message);
}

// 加锁执行一条 SQL（建表、PRAGMA 等）。
void Db::exec(const std::string& sql) {
  std::lock_guard<std::mutex> lock(mu_);
  execUnlocked(sql);
}

// 不加锁执行 SQL。只能在已经持有 mu_ 的代码里调用
//（例如事务 BEGIN/COMMIT/ROLLBACK），否则会破坏串行化。
void Db::execUnlocked(const std::string& sql) {
  char* err = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    std::string message = err ? err : "sql error";
    sqlite3_free(err);
    setError(message + " | " + sql);
    throw std::runtime_error(message);
  }
}

// 建表（全部 IF NOT EXISTS，旧数据库升级时也安全）。
//   roots：扫描根目录
//   tracks：曲目元数据，path 唯一
//   tracks_fts：FTS5 外部内容表，trigram 分词，触发器同步增删改
//   playlists / playlist_items：播放列表，position 决定顺序
// 删除曲目时外键不会自动清 playlist_items（track_id 没有外键），
// 所以 removeRoot/deleteTracksUnderRootNotIn 会显式清理悬空项。
void Db::createSchema() {
  exec(R"SQL(
    CREATE TABLE IF NOT EXISTS roots(
      id INTEGER PRIMARY KEY,
      path TEXT UNIQUE NOT NULL,
      added_at INTEGER NOT NULL DEFAULT 0
    );
    CREATE TABLE IF NOT EXISTS tracks(
      id INTEGER PRIMARY KEY,
      path TEXT UNIQUE NOT NULL,
      title TEXT NOT NULL,
      artist TEXT NOT NULL DEFAULT '',
      album TEXT NOT NULL DEFAULT '',
      codec TEXT NOT NULL DEFAULT '',
      track_no INTEGER NOT NULL DEFAULT 0,
      disc_no INTEGER NOT NULL DEFAULT 0,
      duration_ms INTEGER NOT NULL DEFAULT 0,
      sample_rate INTEGER NOT NULL DEFAULT 0,
      channels INTEGER NOT NULL DEFAULT 0,
      bit_depth INTEGER NOT NULL DEFAULT 0,
      bitrate INTEGER NOT NULL DEFAULT 0,
      mtime INTEGER NOT NULL DEFAULT 0,
      size INTEGER NOT NULL DEFAULT 0,
      added_at INTEGER NOT NULL DEFAULT 0
    );
    CREATE VIRTUAL TABLE IF NOT EXISTS tracks_fts USING fts5(
      title, artist, album,
      content='tracks', content_rowid='id',
      tokenize='trigram'
    );
    CREATE TRIGGER IF NOT EXISTS tracks_ai AFTER INSERT ON tracks BEGIN
      INSERT INTO tracks_fts(rowid, title, artist, album)
      VALUES (new.id, new.title, new.artist, new.album);
    END;
    CREATE TRIGGER IF NOT EXISTS tracks_ad AFTER DELETE ON tracks BEGIN
      INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album)
      VALUES ('delete', old.id, old.title, old.artist, old.album);
    END;
    CREATE TRIGGER IF NOT EXISTS tracks_au AFTER UPDATE ON tracks BEGIN
      INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album)
      VALUES ('delete', old.id, old.title, old.artist, old.album);
      INSERT INTO tracks_fts(rowid, title, artist, album)
      VALUES (new.id, new.title, new.artist, new.album);
    END;
    CREATE TABLE IF NOT EXISTS playlists(
      id INTEGER PRIMARY KEY,
      name TEXT NOT NULL,
      created_at INTEGER NOT NULL DEFAULT 0
    );
    CREATE TABLE IF NOT EXISTS playlist_items(
      playlist_id INTEGER NOT NULL,
      track_id INTEGER NOT NULL,
      position INTEGER NOT NULL DEFAULT 0,
      PRIMARY KEY(playlist_id, track_id),
      FOREIGN KEY(playlist_id) REFERENCES playlists(id) ON DELETE CASCADE
    );
    CREATE INDEX IF NOT EXISTS idx_playlist_items_playlist
      ON playlist_items(playlist_id, position);
  )SQL");
}

// 插入根目录；重复添加不报错，只保留原来的 added_at。
void Db::addRoot(const std::string& path) {
  std::lock_guard<std::mutex> lock(mu_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO roots(path, added_at) VALUES(?, ?) "
      "ON CONFLICT(path) DO UPDATE SET added_at=roots.added_at;";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    throw std::runtime_error(error_);
  }
  sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, nowMs());
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string message = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    setError(message);
    throw std::runtime_error(message);
  }
  sqlite3_finalize(stmt);
}

// 移除根目录：
//   1. 删除 roots 记录
//   2. 重新计算“不在任何根目录下”的曲目并删除（只删索引，不动磁盘文件）
//   3. 清理 playlist_items 中已经不存在的 track_id
//
// 注意：以前这里用 SQL "substr(path,1,?)=?" 删除曲目，参数是 C++ 的
// 字节长度，而 SQLite 的 substr 按字符计数。中文路径（如 D:\音乐）会
// 字节数≠字符数，导致条件不成立、曲目残留。现在统一交给 isUnderRoot
// 的 C++ 前缀比较，避免这类编码问题。
void Db::removeRoot(const std::string& path) {
  std::lock_guard<std::mutex> lock(mu_);
  sqlite3_stmt* delRoot = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM roots WHERE path=?;", -1, &delRoot,
                         nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    throw std::runtime_error(error_);
  }
  sqlite3_bind_text(delRoot, 1, path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(delRoot);
  sqlite3_finalize(delRoot);

  const int64_t pruned = pruneOrphanTracksLocked();
  if (pruned > 0) {
    logMsg("removeRoot: pruned " + std::to_string(pruned) + " orphan tracks");
  }
}

// 删除不属于任何 roots 的曲目。用于：
//   * 修复旧版本在中文路径下移除根目录后留下的孤儿索引
//   * 启动时清理数据库中的脏数据
// 返回删除的曲目数。
int64_t Db::pruneOrphanTracks() {
  std::lock_guard<std::mutex> lock(mu_);
  return pruneOrphanTracksLocked();
}

// 已持有 mu_ 的版本（removeRoot 内部调用）。
int64_t Db::pruneOrphanTracksLocked() {
  // 1. 读取当前所有根目录。
  std::vector<std::string> rootPaths;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT path FROM roots;", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    return 0;
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char* p = sqlite3_column_text(stmt, 0);
    if (p) rootPaths.emplace_back(reinterpret_cast<const char*>(p));
  }
  sqlite3_finalize(stmt);

  // 2. 找出不属于任何根目录的曲目。
  std::vector<int64_t> stale;
  if (sqlite3_prepare_v2(db_, "SELECT id, path FROM tracks;", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    return 0;
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const int64_t id = sqlite3_column_int64(stmt, 0);
    const unsigned char* p = sqlite3_column_text(stmt, 1);
    const std::string path = p ? reinterpret_cast<const char*>(p) : "";
    bool covered = false;
    for (const auto& root : rootPaths) {
      if (isUnderRoot(path, root)) {
        covered = true;
        break;
      }
    }
    if (!covered) stale.push_back(id);
  }
  sqlite3_finalize(stmt);

  if (stale.empty()) return 0;

  // 3. 批量删除（一个事务）。
  sqlite3_stmt* del = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM tracks WHERE id=?;", -1, &del,
                         nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    return 0;
  }
  try {
    execUnlocked("BEGIN IMMEDIATE;");
    for (const int64_t id : stale) {
      sqlite3_reset(del);
      sqlite3_bind_int64(del, 1, id);
      if (sqlite3_step(del) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
      }
    }
    execUnlocked("COMMIT;");
  } catch (...) {
    sqlite3_finalize(del);
    execUnlocked("ROLLBACK;");
    throw;
  }
  sqlite3_finalize(del);

  // 4. 同步清理播放列表中的悬空项。
  execUnlocked("DELETE FROM playlist_items WHERE track_id NOT IN (SELECT id FROM tracks);");
  return static_cast<int64_t>(stale.size());
}

// 返回所有根目录，按路径排序。
std::vector<std::string> Db::roots() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> out;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT path FROM roots ORDER BY path;", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    return out;
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char* p = sqlite3_column_text(stmt, 0);
    if (p) out.emplace_back(reinterpret_cast<const char*>(p));
  }
  sqlite3_finalize(stmt);
  return out;
}

// 按 id 读取单曲；不存在返回 nullopt。
std::optional<TrackRecord> Db::getTrack(int64_t id) {
  std::lock_guard<std::mutex> lock(mu_);
  sqlite3_stmt* stmt = nullptr;
  const std::string sql = std::string("SELECT ") + kTrackColumns +
                          " FROM tracks t WHERE t.id=?;";
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    return std::nullopt;
  }
  sqlite3_bind_int64(stmt, 1, id);
  std::optional<TrackRecord> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) out = readTrack(stmt);
  sqlite3_finalize(stmt);
  return out;
}

// 读取文件的 (mtime, size) 戳；扫描器用它判断是否需要重新解析元数据。
std::optional<std::pair<int64_t, int64_t>> Db::getTrackStamp(const std::string& path) {
  std::lock_guard<std::mutex> lock(mu_);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT mtime, size FROM tracks WHERE path=?;", -1,
                         &stmt, nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    return std::nullopt;
  }
  sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<std::pair<int64_t, int64_t>> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    out = {sqlite3_column_int64(stmt, 0), sqlite3_column_int64(stmt, 1)};
  }
  sqlite3_finalize(stmt);
  return out;
}

// 批量写入/更新曲目。
// 按 path 冲突时只更新元数据，不覆盖 added_at（首次入库时间）。
// 整个批次在一个事务里完成；FTS 由 tracks_ai/au 触发器自动同步。
void Db::upsertTracks(const std::vector<TrackRecord>& tracks) {
  if (tracks.empty()) return;
  std::lock_guard<std::mutex> lock(mu_);
  const char* sql = R"SQL(
    INSERT INTO tracks(path, title, artist, album, codec, track_no, disc_no,
                       duration_ms, sample_rate, channels, bit_depth, bitrate,
                       mtime, size, added_at)
    VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
    ON CONFLICT(path) DO UPDATE SET
      title=excluded.title,
      artist=excluded.artist,
      album=excluded.album,
      codec=excluded.codec,
      track_no=excluded.track_no,
      disc_no=excluded.disc_no,
      duration_ms=excluded.duration_ms,
      sample_rate=excluded.sample_rate,
      channels=excluded.channels,
      bit_depth=excluded.bit_depth,
      bitrate=excluded.bitrate,
      mtime=excluded.mtime,
      size=excluded.size;
  )SQL";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    throw std::runtime_error(error_);
  }
  // 一个批次一个事务。任何一条失败都回滚，避免只写入一半。
  try {
    execUnlocked("BEGIN IMMEDIATE;");
    for (const auto& t : tracks) {
      sqlite3_reset(stmt);
      sqlite3_clear_bindings(stmt);
      sqlite3_bind_text(stmt, 1, t.path.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, t.title.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, t.artist.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4, t.album.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 5, t.codec.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 6, t.trackNo);
      sqlite3_bind_int(stmt, 7, t.discNo);
      sqlite3_bind_int64(stmt, 8, t.durationMs);
      sqlite3_bind_int(stmt, 9, t.sampleRate);
      sqlite3_bind_int(stmt, 10, t.channels);
      sqlite3_bind_int(stmt, 11, t.bitDepth);
      sqlite3_bind_int(stmt, 12, t.bitrate);
      sqlite3_bind_int64(stmt, 13, t.mtime);
      sqlite3_bind_int64(stmt, 14, t.size);
      sqlite3_bind_int64(stmt, 15, t.addedAt ? t.addedAt : nowMs());
      if (sqlite3_step(stmt) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
      }
    }
    execUnlocked("COMMIT;");
  } catch (...) {
    sqlite3_finalize(stmt);
    execUnlocked("ROLLBACK;");
    throw;
  }
  sqlite3_finalize(stmt);
}

// 扫描结束后清理：删除该 root 前缀下、本轮未在 seen 中出现的曲目。
// 实现上先把所有曲目读出来做前缀/集合比较，再批量删除；
// 曲库较大时这也是可接受的（扫描本身就是重活）。
void Db::deleteTracksUnderRootNotIn(const std::string& root,
                                    const std::unordered_set<std::string>& keep) {
  std::lock_guard<std::mutex> lock(mu_);
  sqlite3_stmt* select = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT id, path FROM tracks;", -1, &select,
                         nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    return;
  }
  std::vector<int64_t> stale;
  while (sqlite3_step(select) == SQLITE_ROW) {
    const auto id = sqlite3_column_int64(select, 0);
    const unsigned char* p = sqlite3_column_text(select, 1);
    const std::string path = p ? reinterpret_cast<const char*>(p) : "";
    if (isUnderRoot(path, root) && keep.find(path) == keep.end()) stale.push_back(id);
  }
  sqlite3_finalize(select);

  if (stale.empty()) return;
  sqlite3_stmt* del = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM tracks WHERE id=?;", -1, &del,
                         nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    return;
  }
  try {
    execUnlocked("BEGIN IMMEDIATE;");
    for (int64_t id : stale) {
      sqlite3_reset(del);
      sqlite3_bind_int64(del, 1, id);
      sqlite3_step(del);
    }
    execUnlocked("COMMIT;");
  } catch (...) {
    sqlite3_finalize(del);
    execUnlocked("ROLLBACK;");
    throw;
  }
  sqlite3_finalize(del);
  execUnlocked("DELETE FROM playlist_items WHERE track_id NOT IN (SELECT id FROM tracks);");
}

// 分页查询，前端曲库表、专辑页、文件夹视图都走这里。
//   query 为空：按标题/艺人排序返回全部（或按 root/album 过滤）
//   query >= 3 个字符：先走 FTS5 trigram；没有结果或出错再退回 LIKE
//   query 1~2 个字符：直接 LIKE（trigram 对短查询无效）
// root/album 过滤条件会拼在 WHERE 后，绑定顺序必须与构建 SQL 时一致。
TrackPage Db::page(int64_t offset, int64_t limit, const std::string& query,
                   const std::string& root, const std::string& album) {
  std::lock_guard<std::mutex> lock(mu_);
  TrackPage page;
  if (offset < 0) offset = 0;
  if (limit <= 0) limit = 100;
  if (limit > 1000) limit = 1000;

  const std::string q = trim(query);
  const bool filterRoot = !root.empty();
  const bool filterAlbum = !album.empty();
  // SQLite 的 length(?) 按字符计数，substr 也按字符计数；
  // 不能用 root.size()（UTF-8 字节数），否则中文根目录过滤会失效。
  const std::string filters =
      std::string(filterRoot ? " AND substr(t.path,1,length(?))=?" : "") +
      (filterAlbum ? " AND t.album=?" : "");

  // 按 SQL 中 ? 的出现顺序绑定 root/album。
  // 返回值是下一个可用的参数下标，供 LIMIT/OFFSET 继续绑定。
  auto bindFilters = [&](sqlite3_stmt* stmt, int index) -> int {
    if (filterRoot) {
      // 两个 ?：一个给 length(?)，一个给 substr 的比较值。
      sqlite3_bind_text(stmt, index, root.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, index + 1, root.c_str(), -1, SQLITE_TRANSIENT);
      index += 2;
    }
    if (filterAlbum) {
      sqlite3_bind_text(stmt, index, album.c_str(), -1, SQLITE_TRANSIENT);
      ++index;
    }
    return index;
  };

  if (q.empty()) {
    const std::string countSql =
        "SELECT COUNT(*) FROM tracks t WHERE 1=1" + filters + ";";
    sqlite3_stmt* countStmt = nullptr;
    if (sqlite3_prepare_v2(db_, countSql.c_str(), -1, &countStmt, nullptr) != SQLITE_OK) {
      setError(sqlite3_errmsg(db_));
      return page;
    }
    bindFilters(countStmt, 1);
    if (sqlite3_step(countStmt) == SQLITE_ROW) page.total = sqlite3_column_int64(countStmt, 0);
    sqlite3_finalize(countStmt);

    const std::string sql =
        std::string("SELECT ") + kTrackColumns +
        " FROM tracks t WHERE 1=1" + filters +
        " ORDER BY t.title COLLATE NOCASE, t.artist COLLATE NOCASE LIMIT ? OFFSET ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
      setError(sqlite3_errmsg(db_));
      return page;
    }
    const int index = bindFilters(stmt, 1);
    sqlite3_bind_int64(stmt, index, limit);
    sqlite3_bind_int64(stmt, index + 1, offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) page.tracks.push_back(readTrack(stmt));
    sqlite3_finalize(stmt);
    return page;
  }

  // trigram 分词对 1~2 个字符的查询无效，直接走后面的 LIKE。
  // 3 个字符以上先用 FTS 排序（rank），没有命中再落到 LIKE。
  const bool useFts = utf8Codepoints(q) >= 3;
  if (useFts) {
    const std::string ftsQuery = "\"" + escapeFtsPhrase(q) + "\"";
    const std::string countSql =
        "SELECT COUNT(*) FROM tracks t JOIN tracks_fts ON tracks_fts.rowid=t.id "
        "WHERE tracks_fts MATCH ?" + filters + ";";
    sqlite3_stmt* countStmt = nullptr;
    const int rc = sqlite3_prepare_v2(db_, countSql.c_str(), -1, &countStmt, nullptr);
    if (rc == SQLITE_OK) {
      sqlite3_bind_text(countStmt, 1, ftsQuery.c_str(), -1, SQLITE_TRANSIENT);
      bindFilters(countStmt, 2);
      if (sqlite3_step(countStmt) == SQLITE_ROW) page.total = sqlite3_column_int64(countStmt, 0);
      sqlite3_finalize(countStmt);
    }
    if (rc == SQLITE_OK && page.total > 0) {
      const std::string sql =
          std::string("SELECT ") + kTrackColumns +
          " FROM tracks t JOIN tracks_fts ON tracks_fts.rowid=t.id "
          "WHERE tracks_fts MATCH ?" + filters +
          " ORDER BY rank LIMIT ? OFFSET ?;";
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, ftsQuery.c_str(), -1, SQLITE_TRANSIENT);
        const int index = bindFilters(stmt, 2);
        sqlite3_bind_int64(stmt, index, limit);
        sqlite3_bind_int64(stmt, index + 1, offset);
        while (sqlite3_step(stmt) == SQLITE_ROW) page.tracks.push_back(readTrack(stmt));
        sqlite3_finalize(stmt);
        return page;
      }
      sqlite3_finalize(stmt);
    }
  }

  // Fallback (and short queries): substring match.
  const std::string like = "%" + q + "%";
  const std::string countSql =
      "SELECT COUNT(*) FROM tracks t WHERE "
      "(t.title LIKE ? OR t.artist LIKE ? OR t.album LIKE ?)" + filters + ";";
  sqlite3_stmt* countStmt = nullptr;
  if (sqlite3_prepare_v2(db_, countSql.c_str(), -1, &countStmt, nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    return page;
  }
  for (int i = 1; i <= 3; ++i) {
    sqlite3_bind_text(countStmt, i, like.c_str(), -1, SQLITE_TRANSIENT);
  }
  bindFilters(countStmt, 4);
  page.total = 0;
  if (sqlite3_step(countStmt) == SQLITE_ROW) page.total = sqlite3_column_int64(countStmt, 0);
  sqlite3_finalize(countStmt);

  const std::string sql =
      std::string("SELECT ") + kTrackColumns +
      " FROM tracks t WHERE (t.title LIKE ? OR t.artist LIKE ? OR t.album LIKE ?)" +
      filters + " ORDER BY t.title COLLATE NOCASE LIMIT ? OFFSET ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    return page;
  }
  for (int i = 1; i <= 3; ++i) {
    sqlite3_bind_text(stmt, i, like.c_str(), -1, SQLITE_TRANSIENT);
  }
  const int index = bindFilters(stmt, 4);
  sqlite3_bind_int64(stmt, index, limit);
  sqlite3_bind_int64(stmt, index + 1, offset);
  while (sqlite3_step(stmt) == SQLITE_ROW) page.tracks.push_back(readTrack(stmt));
  sqlite3_finalize(stmt);
  return page;
}

// 曲库统计：曲目总数与去重后的专辑数（空专辑名不计）。
std::pair<int64_t, int64_t> Db::stats() {
  std::lock_guard<std::mutex> lock(mu_);
  int64_t tracks = 0;
  int64_t albums = 0;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM tracks;", -1, &stmt, nullptr) ==
      SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) tracks = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
  }
  if (sqlite3_prepare_v2(db_,
                         "SELECT COUNT(DISTINCT album) FROM tracks WHERE album<>'';",
                         -1, &stmt, nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) albums = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
  }
  return {tracks, albums};
}

// 专辑聚合：按 album 名称分组，取第一首的艺人、曲目数与总时长。
// 同名但不同艺人的专辑目前会合并成一张（M1 的简化）。
std::vector<AlbumInfo> Db::albums() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<AlbumInfo> out;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT album, MIN(artist), COUNT(*), COALESCE(SUM(duration_ms),0) "
      "FROM tracks WHERE album<>'' GROUP BY album ORDER BY album COLLATE NOCASE;";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    return out;
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char* album = sqlite3_column_text(stmt, 0);
    const unsigned char* artist = sqlite3_column_text(stmt, 1);
    AlbumInfo info;
    info.album = album ? reinterpret_cast<const char*>(album) : "";
    info.artist = artist ? reinterpret_cast<const char*>(artist) : "";
    info.trackCount = sqlite3_column_int64(stmt, 2);
    info.durationMs = sqlite3_column_int64(stmt, 3);
    out.push_back(std::move(info));
  }
  sqlite3_finalize(stmt);
  return out;
}

// 所有播放列表 + 每个列表的曲目数（LEFT JOIN 保证空列表也返回）。
std::vector<PlaylistInfo> Db::playlists() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<PlaylistInfo> out;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT p.id, p.name, p.created_at, COUNT(i.track_id) "
      "FROM playlists p LEFT JOIN playlist_items i ON i.playlist_id=p.id "
      "GROUP BY p.id ORDER BY p.created_at, p.id;";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    return out;
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char* name = sqlite3_column_text(stmt, 1);
    PlaylistInfo info;
    info.id = sqlite3_column_int64(stmt, 0);
    info.name = name ? reinterpret_cast<const char*>(name) : "";
    info.createdAt = sqlite3_column_int64(stmt, 2);
    info.trackCount = sqlite3_column_int64(stmt, 3);
    out.push_back(std::move(info));
  }
  sqlite3_finalize(stmt);
  return out;
}

// 读取单个播放列表元信息；不存在返回 nullopt。
std::optional<PlaylistInfo> Db::playlist(int64_t id) {
  std::lock_guard<std::mutex> lock(mu_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT p.id, p.name, p.created_at, COUNT(i.track_id) "
      "FROM playlists p LEFT JOIN playlist_items i ON i.playlist_id=p.id "
      "WHERE p.id=? GROUP BY p.id;";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    return std::nullopt;
  }
  sqlite3_bind_int64(stmt, 1, id);
  std::optional<PlaylistInfo> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char* name = sqlite3_column_text(stmt, 1);
    PlaylistInfo info;
    info.id = sqlite3_column_int64(stmt, 0);
    info.name = name ? reinterpret_cast<const char*>(name) : "";
    info.createdAt = sqlite3_column_int64(stmt, 2);
    info.trackCount = sqlite3_column_int64(stmt, 3);
    out = std::move(info);
  }
  sqlite3_finalize(stmt);
  return out;
}

// 新建播放列表，返回自增 id。
int64_t Db::createPlaylist(const std::string& name) {
  std::lock_guard<std::mutex> lock(mu_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "INSERT INTO playlists(name, created_at) VALUES(?, ?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    throw std::runtime_error(error_);
  }
  sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, nowMs());
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string message = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    setError(message);
    throw std::runtime_error(message);
  }
  const int64_t id = sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(stmt);
  return id;
}

// 重命名播放列表；id 不存在时 UPDATE 影响 0 行，不抛错。
void Db::renamePlaylist(int64_t id, const std::string& name) {
  std::lock_guard<std::mutex> lock(mu_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "UPDATE playlists SET name=? WHERE id=?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    throw std::runtime_error(error_);
  }
  sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, id);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string message = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    setError(message);
    throw std::runtime_error(message);
  }
  sqlite3_finalize(stmt);
}

// 按 trackIds 的顺序重写 position。
// 前端拖拽后会把完整顺序发过来，因此这里不需要做“插入/移动”的位置计算，
// 只要按数组下标更新每一行的 position 即可；整个过程在一个事务里。
void Db::reorderPlaylist(int64_t id, const std::vector<int64_t>& trackIds) {
  std::lock_guard<std::mutex> lock(mu_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "UPDATE playlist_items SET position=? WHERE playlist_id=? AND track_id=?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    throw std::runtime_error(error_);
  }
  // 重排必须原子完成，避免中途失败留下重复 position。
  try {
    execUnlocked("BEGIN IMMEDIATE;");
    for (std::size_t i = 0; i < trackIds.size(); ++i) {
      sqlite3_reset(stmt);
      sqlite3_clear_bindings(stmt);
      sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(i));
      sqlite3_bind_int64(stmt, 2, id);
      sqlite3_bind_int64(stmt, 3, trackIds[i]);
      if (sqlite3_step(stmt) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
      }
    }
    execUnlocked("COMMIT;");
  } catch (...) {
    sqlite3_finalize(stmt);
    execUnlocked("ROLLBACK;");
    throw;
  }
  sqlite3_finalize(stmt);
}

// 删除播放列表及其所有条目（先删 items，再删主表）。
void Db::deletePlaylist(int64_t id) {
  std::lock_guard<std::mutex> lock(mu_);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM playlist_items WHERE playlist_id=?;", -1,
                         &stmt, nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    throw std::runtime_error(error_);
  }
  sqlite3_bind_int64(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (sqlite3_prepare_v2(db_, "DELETE FROM playlists WHERE id=?;", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    throw std::runtime_error(error_);
  }
  sqlite3_bind_int64(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

// 把曲目追加到播放列表末尾。
// 返回 false：播放列表或曲目不存在。
// 重复添加由主键 (playlist_id, track_id) + INSERT OR IGNORE 自动忽略。
bool Db::addTrackToPlaylist(int64_t playlistId, int64_t trackId) {
  std::lock_guard<std::mutex> lock(mu_);

  auto exists = [&](const char* sql, int64_t id) -> bool {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      setError(sqlite3_errmsg(db_));
      return false;
    }
    sqlite3_bind_int64(stmt, 1, id);
    const bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
  };

  if (!exists("SELECT 1 FROM playlists WHERE id=?;", playlistId)) return false;
  if (!exists("SELECT 1 FROM tracks WHERE id=?;", trackId)) return false;

  // 新条目追加到最大 position 之后；空列表从 0 开始。
  int64_t position = 0;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db_, "SELECT COALESCE(MAX(position)+1,0) FROM playlist_items WHERE playlist_id=?;",
          -1, &stmt, nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    return false;
  }
  sqlite3_bind_int64(stmt, 1, playlistId);
  if (sqlite3_step(stmt) == SQLITE_ROW) position = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);

  if (sqlite3_prepare_v2(
          db_,
          "INSERT OR IGNORE INTO playlist_items(playlist_id, track_id, position) "
          "VALUES(?, ?, ?);",
          -1, &stmt, nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    return false;
  }
  sqlite3_bind_int64(stmt, 1, playlistId);
  sqlite3_bind_int64(stmt, 2, trackId);
  sqlite3_bind_int64(stmt, 3, position);
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  if (!ok) setError(sqlite3_errmsg(db_));
  sqlite3_finalize(stmt);
  return ok;
}

// 从播放列表移除一首歌（保留其他曲目的相对顺序，position 不回填）。
void Db::removeTrackFromPlaylist(int64_t playlistId, int64_t trackId) {
  std::lock_guard<std::mutex> lock(mu_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "DELETE FROM playlist_items WHERE playlist_id=? AND track_id=?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    throw std::runtime_error(error_);
  }
  sqlite3_bind_int64(stmt, 1, playlistId);
  sqlite3_bind_int64(stmt, 2, trackId);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

// 按 position 顺序返回播放列表曲目。
// JOIN tracks 会自动跳过已从曲库删除的悬空条目。
std::vector<TrackRecord> Db::playlistTracks(int64_t playlistId) {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<TrackRecord> out;
  sqlite3_stmt* stmt = nullptr;
  const std::string sql = std::string("SELECT ") + kTrackColumns +
                          " FROM tracks t JOIN playlist_items i ON i.track_id=t.id "
                          "WHERE i.playlist_id=? ORDER BY i.position, t.title COLLATE NOCASE;";
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    setError(sqlite3_errmsg(db_));
    return out;
  }
  sqlite3_bind_int64(stmt, 1, playlistId);
  while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(readTrack(stmt));
  sqlite3_finalize(stmt);
  return out;
}

}  // namespace nf
