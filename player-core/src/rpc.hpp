#pragma once

// =============================================================================
// rpc.hpp - stdio 上的 JSON-RPC 2.0 服务
//
// 帧格式与 LSP 相同：
//   Content-Length: <字节数>\r\n
//   \r\n
//   <UTF-8 JSON>
//
// 线程模型：
//   * run() 在调用线程（main）里阻塞读取 stdin，串行处理请求。
//   * notify() 可以被扫描线程、状态线程并发调用；
//     sendJson() 用 outMutex_ 保证一帧不会被多个线程交错写入 stdout。
//   * stdout 只允许 RPC 帧；日志一律走 stderr。
// =============================================================================

#include <atomic>
#include <mutex>
#include <string>

#include "db.hpp"
#include "library.hpp"
#include "nlohmann/json.hpp"
#include "playback.hpp"

namespace nf {

class RpcServer {
 public:
  explicit RpcServer(Db& db);

  // 依赖在 main 中构造完再注入，避免 RpcServer 与 Library/Playback 循环依赖。
  void setLibrary(Library* library) { library_ = library; }
  void setPlayback(Playback* playback) { playback_ = playback; }

  // 阻塞读取 stdin，直到 EOF 或 stop() 被调用。
  void run();
  // 发送一条 JSON-RPC 通知（没有 id，不期待响应）。
  void notify(const std::string& method, const nlohmann::json& params);
  void stop() { running_.store(false); }

 private:
  // 根据 method 分发到具体业务，返回 result；失败抛 RpcError。
  nlohmann::json dispatch(const std::string& method, const nlohmann::json& params);
  // 解析一帧 JSON 并执行/回包。
  void handleBody(const std::string& body);
  // 线程安全地写一帧到 stdout。
  void sendJson(const nlohmann::json& message);
  void sendResult(const nlohmann::json& id, const nlohmann::json& result);
  void sendError(const nlohmann::json& id, int code, const std::string& message);
  // 把 TrackRecord 转成前端使用的 JSON。
  nlohmann::json trackJson(const TrackRecord& track) const;

  Db& db_;
  Library* library_ = nullptr;
  Playback* playback_ = nullptr;
  std::mutex outMutex_;            // 保护 stdout 的整帧写入
  std::atomic<bool> running_{true};
};

}  // namespace nf
