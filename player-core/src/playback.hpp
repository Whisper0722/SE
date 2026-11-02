#pragma once

// =============================================================================
// playback.hpp - 播放引擎
//
// 数据流：
//   RPC 线程 --play/pause/seek--> Playback 状态
//   解码线程：FFmpeg 解码 -> 重采样 -> FloatRingBuffer
//   音频线程：AudioOutput -> FillCallback -> 从环形缓冲取数据
//
// 设计：
//   * 一个解码线程负责整个队列：一首播完自动切下一首。
//   * 队列在 stateMutex_ 保护下读写；位置用 seekBaseMs_ + 已播放帧数计算。
//   * 跳转采用“请求标志”：seek() 先暂停输出并写 seekRequest_，
//     解码线程在循环中看到请求后执行 av_seek_frame、清空环形缓冲并恢复输出。
//   * 当前是共享模式，不是 bit-perfect，也暂不支持无缝播放。
// =============================================================================

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_output.hpp"
#include "db.hpp"
#include "nlohmann/json.hpp"
#include "ring_buffer.hpp"

namespace nf {

class Playback {
 public:
  // forceNullAudio=true 时不打开声卡，用于自动化测试。
  Playback(Db& db, bool forceNullAudio = false);
  ~Playback();

  Playback(const Playback&) = delete;
  Playback& operator=(const Playback&) = delete;

  // 设置播放队列（只是记下 ID 顺序，不会自动开始播放）。
  void setQueue(const std::vector<int64_t>& ids);
  nlohmann::json queueJson() const;

  // 播放指定曲目；若它不在当前队列中，会把队列重置为只有这一首。
  void play(int64_t trackId);
  void pause();
  void resume();
  void toggle();
  void seek(int64_t positionMs);
  void next();
  void prev();
  void setVolume(double volume);

  // 给状态线程/前端使用的快照：曲目、播放状态、位置、时长、音量、队列。
  nlohmann::json snapshotJson() const;
  void shutdown();

 private:
  void decoderLoop();                       // 解码线程主循环（跨队列逐首解码）
  void startDecoder();                      // join 旧线程并启动新解码线程
  void stopDecoder();                       // 置停止位、暂停输出、join
  void startIndex(std::size_t index);       // 按队列下标播放
  bool decodeFile(const std::string& path); // 解码一首歌，处理 seek/EOF
  std::size_t fillAudio(float* dst, std::size_t frames); // 音频回调取数
  int64_t positionMsLocked() const;         // 已持 stateMutex_ 时计算位置
  int64_t positionMs() const;               // 加锁版本
  void setError(const std::string& message);

  Db& db_;
  AudioOutput audio_;
  FloatRingBuffer ring_{262144};  // 约 2.7 秒 @48k 立体声

  // 三类互斥：状态/队列、环形缓冲、解码线程生命周期。
  mutable std::mutex stateMutex_;
  std::mutex ringMutex_;
  std::mutex decoderMutex_;

  std::vector<int64_t> queue_;   // 当前播放队列（track id）
  std::size_t queueIndex_ = 0;   // 当前曲目在队列中的下标
  int64_t currentId_ = 0;        // 当前曲目 id
  std::string currentPath_;      // 当前文件路径（给解码线程）
  int64_t durationMs_ = 0;       // 当前曲目时长
  std::string lastError_;        // 最近一次错误（前端会显示）

  // 以下原子变量可以无锁读取，供音频回调和状态线程使用。
  std::atomic<bool> playing_{false};       // 是否处于“播放”语义
  std::atomic<bool> paused_{true};         // 是否暂停
  std::atomic<bool> decoderActive_{false}; // 解码线程是否还活着
  std::atomic<bool> decoderStop_{false};   // 请求解码线程退出
  std::atomic<bool> shuttingDown_{false};
  std::atomic<int64_t> seekRequest_{-1};   // >=0 表示有待处理的跳转目标(ms)
  std::atomic<int64_t> seekBaseMs_{0};     // 跳转后位置的基准
  std::atomic<int64_t> framesPlayed_{0};   // 本次跳转后已消费的帧数
  std::atomic<double> volume_{0.8};

  std::thread decoder_;
};

}  // namespace nf
