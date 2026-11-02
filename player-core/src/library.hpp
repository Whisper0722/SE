#pragma once

// =============================================================================
// library.hpp - 曲库扫描器
//
// 工作方式：
//   1. addRoot() 校验目录 -> 写入 roots 表 -> 异步启动扫描
//   2. scanAsync() 起一个后台线程，串行扫描所有根目录
//   3. scanRoot() 递归遍历文件 -> TagLib 读元数据 -> 批量写 DB
//   4. 扫描过程中通过 Notify 回调把 scan.progress 发给 RpcServer
//   5. 扫描结束发送 library.changed，前端据此刷新
//
// 线程：扫描在独立线程中执行；Db 自身有锁，因此扫描与 RPC 查询可以并发。
// =============================================================================

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "db.hpp"
#include "nlohmann/json.hpp"

namespace nf {

class Library {
 public:
  // 事件回调：method 例如 "scan.progress"/"library.changed"，payload 是 JSON。
  using Notify = std::function<void(const std::string&, const nlohmann::json&)>;

  Library(Db& db, Notify notify);
  ~Library();

  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;

  // 规范化路径、写入 roots，并立即开始一次异步扫描。
  void addRoot(const std::string& path);
  // 删除根目录及其曲目索引，并通知前端刷新。
  void removeRoot(const std::string& path);
  // 如果当前没有扫描任务，启动一个新的后台扫描线程。
  void scanAsync();
  bool isScanning() const { return scanning_.load(); }
  // 停止扫描并 join 后台线程（析构时自动调用）。
  void shutdown();

 private:
  void scanWorker();                       // 后台线程入口：遍历所有根目录
  void scanRoot(const std::string& root);  // 扫描单个根目录
  void notify(const std::string& method, const nlohmann::json& payload) const;

  Db& db_;
  Notify notify_;
  std::atomic<bool> scanning_{false};  // 是否有扫描在跑
  std::atomic<bool> stop_{false};      // shutdown 时置位，循环中检查
  std::thread worker_;
  mutable std::mutex workerMutex_;     // 保护 worker_ 的创建/join
};

}  // namespace nf
