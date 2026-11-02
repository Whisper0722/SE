// =============================================================================
// main.cpp - player-core 进程入口
//
// 启动顺序：
//   1. 把 stdin/stdout 切到二进制模式（Windows 下必须，否则 \r\n 会被转换）
//   2. 解析 --db / --audio 参数，创建数据库
//   3. 创建 RpcServer、Library、Playback 并互相注入依赖
//   4. 启动状态线程，每 250ms 推送一次 playback.state
//   5. 进入 RpcServer::run()，阻塞读取 stdin 上的 JSON-RPC 帧
//   6. stdin 关闭后停止状态线程、关闭播放和扫描线程
//
// 注意：stdout 只用于 RPC 帧，所有日志都走 stderr。
// =============================================================================

#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "db.hpp"
#include "library.hpp"
#include "playback.hpp"
#include "rpc.hpp"
#include "util.hpp"

namespace {

// 命令行参数。Electron 主进程会传入 --db <userData>/library.db。
struct Options {
  std::string dbPath = "nightfall.db";
  bool nullAudio = false;  // --audio null 时用测试输出，不打开声卡
  bool help = false;
};

// 解析 argv。只支持几个简单参数，未知参数忽略。
Options parseArgs(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--db" && i + 1 < argc) {
      options.dbPath = argv[++i];
    } else if (arg == "--audio" && i + 1 < argc) {
      options.nullAudio = std::string(argv[++i]) == "null";
    } else if (arg == "--help" || arg == "-h") {
      options.help = true;
    }
  }
  return options;
}

void printHelp() {
  std::cerr << "player-core [--db <path>] [--audio wasapi|null]\n"
            << "  Speaks JSON-RPC 2.0 over stdin/stdout with Content-Length framing.\n";
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  // Windows 文本模式会把 \n 转成 \r\n、把 Ctrl+Z 当 EOF；
  // 我们的帧是二进制协议，必须显式切到 O_BINARY。
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);

  const Options options = parseArgs(argc, argv);
  if (options.help) {
    printHelp();
    return 0;
  }

  try {
    // 确保数据库文件所在目录存在（Electron 的 userData 目录通常已存在）。
    const auto dbPath = nf::utf8ToPath(options.dbPath);
    if (dbPath.has_parent_path()) {
      std::error_code ec;
      std::filesystem::create_directories(dbPath.parent_path(), ec);
    }

    nf::logMsg("nightfall-core 0.1.0 starting");
    nf::logMsg("db: " + options.dbPath);

    // 依赖注入：RpcServer 需要 Db/Library/Playback；
    // Library 通过回调把扫描事件交给 RpcServer 转发给前端。
    nf::Db db(options.dbPath);

    // 启动时清理“不属于任何根目录”的孤儿曲目。
    // 旧版本移除中文路径根目录时可能留下这种残留；清理后列表不会再出现
    // 已无法播放、也无法通过根目录删除的歌曲。
    const int64_t orphanTracks = db.pruneOrphanTracks();
    if (orphanTracks > 0) {
      nf::logMsg("pruned orphan tracks: " + std::to_string(orphanTracks));
    }

    nf::RpcServer server(db);
    nf::Library library(db, [&server](const std::string& method,
                                      const nlohmann::json& payload) {
      server.notify(method, payload);
    });
    nf::Playback playback(db, options.nullAudio);
    server.setLibrary(&library);
    server.setPlayback(&playback);

    // 数据库里已有根目录时，启动后自动后台扫描一次，保证曲库新鲜。
    if (!db.roots().empty()) library.scanAsync();

    // 状态线程：定时把播放快照推给前端。
    // 前端不轮询 playback.state，而是依赖这个通知流更新进度条。
    std::atomic<bool> statusRunning{true};
    std::thread statusThread([&server, &playback, &statusRunning] {
      while (statusRunning.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!statusRunning.load()) break;
        server.notify("playback.state", playback.snapshotJson());
      }
    });

    // 阻塞在这里读取 RPC，直到 Electron 关闭 stdin 或发送 system.shutdown。
    server.run();

    statusRunning.store(false);
    if (statusThread.joinable()) statusThread.join();
    playback.shutdown();
    library.shutdown();
    nf::logMsg("nightfall-core stopped");
    return 0;
  } catch (const std::exception& e) {
    nf::logMsg(std::string("fatal: ") + e.what());
    return 1;
  }
}
