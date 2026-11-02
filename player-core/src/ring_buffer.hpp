#pragma once

// =============================================================================
// ring_buffer.hpp - 解码线程与音频回调之间的无锁环形缓冲
//
// 数据流：
//   FFmpeg 解码线程（唯一生产者） --write--> [浮点样本环形缓冲] --read--> WASAPI 回调线程（唯一消费者）
//
// 设计要点：
//   * 容量必须是 2 的幂，这样用掩码 & 就能完成取模，避免昂贵的除法。
//   * write_/read_ 是单调递增的“总写入/总读出样本数”，不是数组下标；
//     真正下标是 (pos & mask_)。
//   * 使用 acquire/release 内存序，保证生产者写入的数据在消费者读取前可见。
//   * 单生产者/单消费者（SPSC）不需要互斥锁；但 clear() 必须由外部保证
//     消费者当前不在读（Playback 会先暂停音频输出，再加锁清空）。
// =============================================================================

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace nf {

// 交错（interleaved）float 样本的 SPSC 环形缓冲。
class FloatRingBuffer {
 public:
  // capacityPowerOfTwo 必须是 2 的幂；内部数组长度即容量（样本数，不是帧数）。
  explicit FloatRingBuffer(std::size_t capacityPowerOfTwo)
      : mask_(capacityPowerOfTwo - 1), data_(capacityPowerOfTwo, 0.0f) {}

  // 总容量，单位是 float 样本。
  std::size_t capacity() const { return data_.size(); }

  // 当前可读样本数。write_ 只会增加，read_ 只会增加，
  // 两者差值就是尚未消费的样本数。
  std::size_t available() const {
    const std::size_t w = write_.load(std::memory_order_acquire);
    const std::size_t r = read_.load(std::memory_order_acquire);
    return w - r;
  }

  // 当前还能写入多少样本。
  std::size_t freeSpace() const { return data_.size() - available(); }

  // 生产：尽量写入 count 个样本，返回实际写入数（空间不足时可能 < count）。
  // 调用者（解码线程）在返回 0 时应短暂休眠，避免忙等。
  std::size_t write(const float* src, std::size_t count) {
    const std::size_t w = write_.load(std::memory_order_relaxed);
    const std::size_t r = read_.load(std::memory_order_acquire);
    const std::size_t freeCount = data_.size() - (w - r);
    const std::size_t n = count < freeCount ? count : freeCount;
    if (n == 0) return 0;

    // 环形数组可能在中途回绕：先写从 (w & mask_) 到数组末尾的一段，
    // 再把剩余部分写到数组开头。
    std::size_t first = data_.size() - (w & mask_);
    if (first > n) first = n;
    std::memcpy(data_.data() + (w & mask_), src, first * sizeof(float));
    if (n > first) {
      std::memcpy(data_.data(), src + first, (n - first) * sizeof(float));
    }
    write_.store(w + n, std::memory_order_release);
    return n;
  }

  // 消费：尽量读出 count 个样本，返回实际读出数（数据不足时可能 < count）。
  // 音频回调线程不等待：读不够的部分由调用方补静音。
  std::size_t read(float* dst, std::size_t count) {
    const std::size_t r = read_.load(std::memory_order_relaxed);
    const std::size_t w = write_.load(std::memory_order_acquire);
    const std::size_t avail = w - r;
    const std::size_t n = count < avail ? count : avail;
    if (n == 0) return 0;

    std::size_t first = data_.size() - (r & mask_);
    if (first > n) first = n;
    std::memcpy(dst, data_.data() + (r & mask_), first * sizeof(float));
    if (n > first) {
      std::memcpy(dst + first, data_.data(), (n - first) * sizeof(float));
    }
    read_.store(r + n, std::memory_order_release);
    return n;
  }

  // 清空缓冲（跳转、切歌时使用）。
  // 调用前提：音频输出已暂停、解码线程已停止或已收到 seek 请求，
  // 否则可能和正在读写的线程产生竞争。Playback 用 ringMutex_ 保护这一步。
  void clear() {
    read_.store(0, std::memory_order_release);
    write_.store(0, std::memory_order_release);
  }

 private:
  std::size_t mask_;              // 容量 - 1，用于快速取模
  std::vector<float> data_;       // 交错 float 样本
  std::atomic<std::size_t> write_{0};  // 生产者写入总样本数
  std::atomic<std::size_t> read_{0};   // 消费者读取总样本数
};

}  // namespace nf
