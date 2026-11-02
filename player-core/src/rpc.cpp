// =============================================================================
// rpc.cpp - stdio 上的 JSON-RPC 2.0 实现
//
// 每个帧由 Content-Length 头 + 空行 + UTF-8 JSON 组成（与 LSP 相同），
// 这样 stdin/stdout 可以安全传输含空格、换行、中文的 JSON。
//
// 请求：{"jsonrpc":"2.0","id":1,"method":"...","params":{...}}
// 响应：{"jsonrpc":"2.0","id":1,"result":{...}} 或 {"error":{...}}
// 通知：没有 id，只用于后端 -> 前端推送（scan.progress/playback.state）。
// =============================================================================

#include "rpc.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <stdexcept>

#include "util.hpp"

namespace nf {
namespace {

// 业务错误。code 会直接写进 JSON-RPC 的 error.code，
// 例如 -32602 表示参数错误、-32001 表示资源不存在。
struct RpcError : std::runtime_error {
  int code;
  RpcError(int code_, const std::string& message)
      : std::runtime_error(message), code(code_) {}
};

// 把 HTTP 风格的头部名转小写，便于大小写不敏感地识别 Content-Length。
std::string lowerAscii(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// 读取可选的整数参数；缺失或类型不对时返回 fallback。
int64_t intParam(const nlohmann::json& params, const char* key, int64_t fallback = 0) {
  const auto it = params.find(key);
  if (it == params.end() || !it->is_number_integer()) return fallback;
  return it->get<int64_t>();
}

// 读取必填字符串参数；缺失或类型不对时抛 -32602。
std::string stringParam(const nlohmann::json& params, const char* key) {
  const auto it = params.find(key);
  if (it == params.end() || !it->is_string()) {
    throw RpcError(-32602, std::string("missing string param: ") + key);
  }
  return it->get<std::string>();
}

// 读取可选的浮点参数；缺失时返回 fallback。
double doubleParam(const nlohmann::json& params, const char* key, double fallback = 0.0) {
  const auto it = params.find(key);
  if (it == params.end() || !it->is_number()) return fallback;
  return it->get<double>();
}

// 去掉首尾空白；播放列表名不能只是空格。
std::string trimText(const std::string& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

// PlaylistInfo -> 前端 JSON。
nlohmann::json playlistJson(const PlaylistInfo& playlist) {
  return {{"id", playlist.id},
          {"name", playlist.name},
          {"trackCount", playlist.trackCount},
          {"createdAt", playlist.createdAt}};
}

}  // namespace

RpcServer::RpcServer(Db& db) : db_(db) {}

// 读取 stdin 的主循环。
// 先逐行读头部，遇到空行后按 Content-Length 精确读取 body。
// 解析/读取失败会直接退出线程（Electron 会重启核心）。
void RpcServer::run() {
  std::string line;
  long long contentLength = -1;

  while (running_.load()) {
    if (!std::getline(std::cin, line)) break;
    // 文本模式下可能带 \r；二进制模式下也可能，手动去掉。
    if (!line.empty() && line.back() == '\r') line.pop_back();

    if (line.empty()) {
      // 空行 = 头部结束，接下来是 body。
      if (contentLength < 0) continue;
      // 防御异常大帧，避免一次性分配过多内存。
      if (contentLength > 32 * 1024 * 1024) {
        logMsg("rpc: frame too large");
        break;
      }
      std::string body(static_cast<std::size_t>(contentLength), '\0');
      std::cin.read(body.data(), contentLength);
      if (std::cin.gcount() != contentLength) break;  // 半帧：输入被关闭
      handleBody(body);
      contentLength = -1;
      continue;
    }

    // 解析头部，只关心 Content-Length；其他头（如 Content-Type）忽略。
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
      const std::string name = lowerAscii(line.substr(0, colon));
      if (name == "content-length") {
        try {
          contentLength = std::stoll(line.substr(colon + 1));
        } catch (...) {
          contentLength = -1;
        }
      }
    }
  }
  logMsg("rpc: input closed");
}

// 处理一帧 body：解析 JSON -> 校验 -> 分发 -> 回包。
// 没有 id 的消息视为通知，只执行不回包。
void RpcServer::handleBody(const std::string& body) {
  nlohmann::json message;
  try {
    message = nlohmann::json::parse(body);
  } catch (const std::exception& e) {
    sendError(nullptr, -32700, std::string("parse error: ") + e.what());
    return;
  }

  if (!message.is_object()) {
    sendError(nullptr, -32600, "invalid request");
    return;
  }

  const bool hasId = message.contains("id");
  const nlohmann::json id = hasId ? message["id"] : nlohmann::json(nullptr);
  const std::string method = message.value("method", "");
  const nlohmann::json params =
      message.contains("params") ? message["params"] : nlohmann::json::object();

  if (method.empty()) {
    if (hasId) sendError(id, -32600, "missing method");
    return;
  }

  try {
    const nlohmann::json result = dispatch(method, params);
    if (hasId) sendResult(id, result);
  } catch (const RpcError& e) {
    if (hasId) sendError(id, e.code, e.what());
  } catch (const std::exception& e) {
    logMsg("rpc: " + method + " failed: " + e.what());
    if (hasId) sendError(id, -32603, e.what());
  }
}

// 数据库记录 -> 前端 JSON。前端不需要 addedAt/mtime/size 等内部字段。
nlohmann::json RpcServer::trackJson(const TrackRecord& track) const {
  return {{"id", track.id},
          {"path", track.path},
          {"title", track.title},
          {"artist", track.artist},
          {"album", track.album},
          {"codec", track.codec},
          {"trackNo", track.trackNo},
          {"discNo", track.discNo},
          {"durationMs", track.durationMs},
          {"sampleRate", track.sampleRate},
          {"channels", track.channels},
          {"bitrate", track.bitrate}};
}

// 方法分发。方法名按 "模块.动作" 命名：
//   library.*   曲库/扫描/搜索
//   playlists.* 播放列表 CRUD 与排序
//   queue.*     播放队列
//   playback.*  播放控制
//   system.*    进程控制
// 未找到时抛 -32601。
nlohmann::json RpcServer::dispatch(const std::string& method,
                                   const nlohmann::json& params) {
  // ---- 健康检查 ----
  if (method == "ping") {
    return {{"ok", true}, {"version", "0.1.0"}};
  }

  // ---- 曲库 ----
  if (method == "library.roots") {
    nlohmann::json roots = nlohmann::json::array();
    for (const auto& root : db_.roots()) roots.push_back(root);
    return {{"roots", roots}};
  }

  if (method == "library.addRoot") {
    if (!library_) throw RpcError(-32603, "library not ready");
    const std::string path = stringParam(params, "path");
    library_->addRoot(path);
    return {{"ok", true}, {"path", path}};
  }

  if (method == "library.removeRoot") {
    if (!library_) throw RpcError(-32603, "library not ready");
    const std::string path = stringParam(params, "path");
    library_->removeRoot(path);
    return {{"ok", true}};
  }

  if (method == "library.scan") {
    if (!library_) throw RpcError(-32603, "library not ready");
    library_->scanAsync();
    return {{"started", library_->isScanning()}};
  }

  if (method == "library.track") {
    const int64_t id = intParam(params, "id", 0);
    const auto track = db_.getTrack(id);
    if (!track) throw RpcError(-32001, "track not found");
    return trackJson(*track);
  }

  if (method == "library.page") {
    const int64_t offset = intParam(params, "offset", 0);
    const int64_t limit = intParam(params, "limit", 100);
    const std::string query = params.value("query", std::string());
    const std::string root = params.value("root", std::string());
    const std::string album = params.value("album", std::string());
    const TrackPage page = db_.page(offset, limit, query, root, album);
    nlohmann::json tracks = nlohmann::json::array();
    for (const auto& track : page.tracks) tracks.push_back(trackJson(track));
    return {{"total", page.total}, {"offset", offset}, {"limit", limit},
            {"tracks", tracks}};
  }

  if (method == "library.stats") {
    const auto [tracks, albums] = db_.stats();
    return {{"tracks", tracks}, {"albums", albums}};
  }

  if (method == "library.albums") {
    nlohmann::json albums = nlohmann::json::array();
    for (const auto& album : db_.albums()) {
      albums.push_back({{"album", album.album},
                        {"artist", album.artist},
                        {"trackCount", album.trackCount},
                        {"durationMs", album.durationMs}});
    }
    return {{"albums", albums}};
  }

  // ---- 播放列表 ----
  if (method == "playlists.list") {
    nlohmann::json playlists = nlohmann::json::array();
    for (const auto& playlist : db_.playlists()) {
      playlists.push_back(playlistJson(playlist));
    }
    return {{"playlists", playlists}};
  }

  if (method == "playlists.create") {
    const std::string name = trimText(stringParam(params, "name"));
    if (name.empty()) throw RpcError(-32602, "playlist name is empty");
    if (name.size() > 120) throw RpcError(-32602, "playlist name is too long");
    const int64_t id = db_.createPlaylist(name);
    return {{"id", id}, {"name", name}};
  }

  if (method == "playlists.rename") {
    const int64_t id = intParam(params, "id", 0);
    if (!db_.playlist(id)) throw RpcError(-32001, "playlist not found");
    const std::string name = trimText(stringParam(params, "name"));
    if (name.empty()) throw RpcError(-32602, "playlist name is empty");
    if (name.size() > 120) throw RpcError(-32602, "playlist name is too long");
    db_.renamePlaylist(id, name);
    return {{"ok", true}, {"name", name}};
  }

  if (method == "playlists.delete") {
    db_.deletePlaylist(intParam(params, "id", 0));
    return {{"ok", true}};
  }

  if (method == "playlists.reorder") {
    const int64_t id = intParam(params, "id", 0);
    if (!db_.playlist(id)) throw RpcError(-32001, "playlist not found");
    const auto it = params.find("trackIds");
    if (it == params.end() || !it->is_array()) {
      throw RpcError(-32602, "missing trackIds array");
    }
    std::vector<int64_t> trackIds;
    trackIds.reserve(it->size());
    for (const auto& value : *it) {
      if (!value.is_number_integer()) {
        throw RpcError(-32602, "trackIds must be integers");
      }
      trackIds.push_back(value.get<int64_t>());
    }
    db_.reorderPlaylist(id, trackIds);
    return {{"ok", true}};
  }

  if (method == "playlists.addTrack") {
    const int64_t playlistId = intParam(params, "playlistId", 0);
    const int64_t trackId = intParam(params, "trackId", 0);
    if (!db_.addTrackToPlaylist(playlistId, trackId)) {
      throw RpcError(-32001, "playlist or track not found");
    }
    return {{"ok", true}};
  }

  if (method == "playlists.removeTrack") {
    db_.removeTrackFromPlaylist(intParam(params, "playlistId", 0),
                                intParam(params, "trackId", 0));
    return {{"ok", true}};
  }

  if (method == "playlists.tracks") {
    const int64_t id = intParam(params, "id", 0);
    const auto info = db_.playlist(id);
    if (!info) throw RpcError(-32001, "playlist not found");
    nlohmann::json tracks = nlohmann::json::array();
    for (const auto& track : db_.playlistTracks(id)) {
      tracks.push_back(trackJson(track));
    }
    return {{"playlist", playlistJson(*info)}, {"tracks", tracks}};
  }

  // ---- 播放（需要 Playback 就绪）----
  if (!playback_) throw RpcError(-32603, "playback not ready");

  if (method == "queue.set") {
    const auto it = params.find("ids");
    if (it == params.end() || !it->is_array()) {
      throw RpcError(-32602, "missing ids array");
    }
    std::vector<int64_t> ids;
    ids.reserve(it->size());
    for (const auto& value : *it) {
      if (!value.is_number_integer()) throw RpcError(-32602, "ids must be integers");
      ids.push_back(value.get<int64_t>());
    }
    playback_->setQueue(ids);
    return playback_->queueJson();
  }

  if (method == "queue.get") return playback_->queueJson();

  if (method == "playback.play") {
    const int64_t trackId = intParam(params, "trackId", 0);
    if (trackId == 0) throw RpcError(-32602, "missing trackId");
    playback_->play(trackId);
    return playback_->snapshotJson();
  }

  if (method == "playback.pause") {
    playback_->pause();
    return playback_->snapshotJson();
  }

  if (method == "playback.resume") {
    playback_->resume();
    return playback_->snapshotJson();
  }

  if (method == "playback.toggle") {
    playback_->toggle();
    return playback_->snapshotJson();
  }

  if (method == "playback.seek") {
    playback_->seek(intParam(params, "positionMs", 0));
    return playback_->snapshotJson();
  }

  if (method == "playback.next") {
    playback_->next();
    return playback_->snapshotJson();
  }

  if (method == "playback.prev") {
    playback_->prev();
    return playback_->snapshotJson();
  }

  if (method == "playback.setVolume") {
    playback_->setVolume(doubleParam(params, "volume", 0.8));
    return playback_->snapshotJson();
  }

  if (method == "playback.state") return playback_->snapshotJson();

  // ---- 系统 ----
  if (method == "system.shutdown") {
    running_.store(false);
    return {{"ok", true}};
  }

  throw RpcError(-32601, "method not found: " + method);
}

// 写一帧到 stdout。
// 注意 Content-Length 必须是 UTF-8 字节数（body.size()），不是字符数。
// outMutex_ 保证扫描线程/状态线程/请求线程不会把两帧写串。
void RpcServer::sendJson(const nlohmann::json& message) {
  const std::string body = message.dump();
  std::lock_guard<std::mutex> lock(outMutex_);
  std::cout << "Content-Length: " << body.size() << "\r\n\r\n";
  std::cout.write(body.data(), static_cast<std::streamsize>(body.size()));
  std::cout.flush();
}

void RpcServer::sendResult(const nlohmann::json& id, const nlohmann::json& result) {
  sendJson({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

void RpcServer::sendError(const nlohmann::json& id, int code,
                          const std::string& message) {
  sendJson({{"jsonrpc", "2.0"},
            {"id", id},
            {"error", {{"code", code}, {"message", message}}}});
}

// 后端主动推送（没有 id）：扫描进度、library.changed、playback.state。
void RpcServer::notify(const std::string& method, const nlohmann::json& params) {
  sendJson({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
}

}  // namespace nf
