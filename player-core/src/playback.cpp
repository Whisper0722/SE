// =============================================================================
// playback.cpp - 播放引擎实现
//
// 线程与数据流：
//   RPC 线程：调用 play/pause/seek/next...
//   解码线程：decoderLoop() 逐首解码，FFmpeg -> Swr -> FloatRingBuffer
//   音频线程：AudioOutput 调 fillAudio()，从环形缓冲取 float 样本
//   状态线程（main 中）：每 250ms 调 snapshotJson() 推送前端
//
// 位置计算：position = seekBaseMs + framesPlayed * 1000 / sampleRate
//   framesPlayed 由音频回调在真正消费样本时累加，因此暂停时位置自然冻结。
//
// seek 协议：
//   seek() 先暂停输出，设置 seekBase/framesPlayed=0/seekRequest；
//   解码线程看到 seekRequest 后执行 av_seek_frame、清空缓冲、恢复输出。
//   先暂停再写请求可以避免“解码线程提前 resume”和 seek() 的 pause 竞争。
// =============================================================================

#include "playback.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include "util.hpp"

namespace nf {

// 初始化输出，并把 fillAudio 注册给音频线程。
// 构造函数不启动解码线程；真正播放发生在 play()。
Playback::Playback(Db& db, bool forceNullAudio) : db_(db) {
  audio_.initialize(forceNullAudio);
  audio_.setFill([this](float* dst, std::size_t frames) {
    return fillAudio(dst, frames);
  });
  audio_.setVolume(static_cast<float>(volume_.load()));
}

Playback::~Playback() { shutdown(); }

// 幂等关闭：exchange 保证只执行一次，避免重复 join/释放。
void Playback::shutdown() {
  if (shuttingDown_.exchange(true)) return;
  stopDecoder();
  audio_.shutdown();
}

// 保存错误并打日志；前端会通过 playback.state 的 error 字段看到。
void Playback::setError(const std::string& message) {
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    lastError_ = message;
  }
  logMsg("playback error: " + message);
}

// 替换播放队列。如果当前曲目仍在新队列中，保持它的下标；
// 否则从第 0 首开始。这里只改状态，不会自动播放。
void Playback::setQueue(const std::vector<int64_t>& ids) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  queue_ = ids;
  queueIndex_ = 0;
  if (currentId_ != 0) {
    const auto it = std::find(queue_.begin(), queue_.end(), currentId_);
    if (it != queue_.end()) queueIndex_ = static_cast<std::size_t>(it - queue_.begin());
  }
}

// 队列快照：ids + 当前下标。
nlohmann::json Playback::queueJson() const {
  std::lock_guard<std::mutex> lock(stateMutex_);
  nlohmann::json ids = nlohmann::json::array();
  for (const auto id : queue_) ids.push_back(id);
  return {{"ids", ids}, {"index", queueIndex_}};
}

// 启动解码线程。必须与 stopDecoder 串行，避免同时存在两个线程。
void Playback::startDecoder() {
  std::lock_guard<std::mutex> lock(decoderMutex_);
  if (decoder_.joinable()) decoder_.join();
  decoderStop_.store(false);
  decoderActive_.store(true);
  decoder_ = std::thread([this] { decoderLoop(); });
}

// 停止解码线程：
//   1. 置停止位并暂停音频输出（避免回调继续消费旧数据）
//   2. join 解码线程
//   3. 清空 seek 请求和环形缓冲
// 之后调用者可以安全地修改 current/queue 并重新 startDecoder。
void Playback::stopDecoder() {
  decoderStop_.store(true);
  audio_.pause();
  {
    std::lock_guard<std::mutex> lock(decoderMutex_);
    if (decoder_.joinable()) decoder_.join();
  }
  decoderActive_.store(false);
  decoderStop_.store(false);
  seekRequest_.store(-1);
  std::lock_guard<std::mutex> lock(ringMutex_);
  ring_.clear();
}

// 播放指定曲目。
// 若曲目不在队列中，会把队列重置为只有这一首；
// 否则记录它在队列中的下标，后续 next/prev/自动切歌都基于该队列。
void Playback::play(int64_t trackId) {
  const auto track = db_.getTrack(trackId);
  if (!track) throw std::runtime_error("track not found: " + std::to_string(trackId));

  stopDecoder();
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto it = std::find(queue_.begin(), queue_.end(), trackId);
    if (it == queue_.end()) {
      queue_ = {trackId};
      queueIndex_ = 0;
    } else {
      queueIndex_ = static_cast<std::size_t>(it - queue_.begin());
    }
    currentId_ = trackId;
    currentPath_ = track->path;
    durationMs_ = track->durationMs;
    lastError_.clear();
  }

  framesPlayed_.store(0);
  seekBaseMs_.store(0);
  seekRequest_.store(-1);
  paused_.store(false);
  playing_.store(true);
  audio_.setVolume(static_cast<float>(volume_.load()));
  audio_.pause();
  {
    std::lock_guard<std::mutex> lock(ringMutex_);
    ring_.clear();
  }
  if (!audio_.start()) {
    setError("failed to start audio output");
    playing_.store(false);
    return;
  }
  startDecoder();
}

// 按队列下标播放；越界时什么都不做。
void Playback::startIndex(std::size_t index) {
  int64_t id = 0;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (index >= queue_.size()) return;
    id = queue_[index];
  }
  play(id);
}

// 暂停：停止音频客户端，但保留解码线程和缓冲。
// 位置计算依赖“已消费帧数”，音频不再消费后位置自然冻结。
void Playback::pause() {
  if (currentId_ == 0) return;
  paused_.store(true);
  playing_.store(false);
  audio_.pause();
}

// 恢复播放。
// 如果解码线程已经结束（例如播完队列/发生错误），从当前下标重新开始。
void Playback::resume() {
  if (currentId_ == 0) return;
  if (!decoderActive_.load()) {
    std::size_t index;
    {
      std::lock_guard<std::mutex> lock(stateMutex_);
      index = queueIndex_;
    }
    startIndex(index);
    return;
  }
  paused_.store(false);
  playing_.store(true);
  audio_.start();
}

// 播放/暂停切换。
void Playback::toggle() {
  if (playing_.load()) {
    pause();
  } else {
    resume();
  }
}

// 请求跳转。注意这里不直接操作 FFmpeg：
// 只暂停输出并写下 seekRequest，真正的 av_seek_frame 由解码线程执行。
// 这样所有 FFmpeg 调用都集中在解码线程，避免多线程访问同一个 AVFormatContext。
void Playback::seek(int64_t positionMs) {
  int64_t duration = 0;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    duration = durationMs_;
  }
  if (positionMs < 0) positionMs = 0;
  if (duration > 0 && positionMs > duration) positionMs = duration;
  if (currentId_ == 0) return;
  audio_.pause();
  seekBaseMs_.store(positionMs);
  framesPlayed_.store(0);
  seekRequest_.store(positionMs);
}

// 下一首（循环到队首）。
void Playback::next() {
  std::size_t index = 0;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (queue_.empty()) return;
    index = (queueIndex_ + 1) % queue_.size();
  }
  startIndex(index);
}

// 上一首：播放超过 3 秒时先回到本曲开头，否则切到上一首。
void Playback::prev() {
  if (positionMs() > 3000) {
    seek(0);
    return;
  }
  std::size_t index = 0;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (queue_.empty()) return;
    index = queueIndex_ == 0 ? queue_.size() - 1 : queueIndex_ - 1;
  }
  startIndex(index);
}

// 音量同时更新引擎快照和 AudioOutput 的实际增益。
void Playback::setVolume(double volume) {
  volume = std::clamp(volume, 0.0, 1.0);
  volume_.store(volume);
  audio_.setVolume(static_cast<float>(volume));
}

// 计算当前播放位置（调用者已持有 stateMutex_）。
// 只统计真正被音频设备消费的帧，所以暂停/欠载时不会虚增。
int64_t Playback::positionMsLocked() const {
  const int rate = audio_.sampleRate();
  if (rate <= 0) return 0;
  int64_t pos = seekBaseMs_.load() + framesPlayed_.load() * 1000 / rate;
  if (pos < 0) pos = 0;
  if (durationMs_ > 0 && pos > durationMs_) pos = durationMs_;
  return pos;
}

// 加锁版本的位置查询，供 prev() 等外部方法使用。
int64_t Playback::positionMs() const {
  std::lock_guard<std::mutex> lock(stateMutex_);
  return positionMsLocked();
}

// 生成前端播放状态快照。
// 状态线程每 250ms 调用一次；字段名与前端 types.ts 的 PlayerState 对应。
nlohmann::json Playback::snapshotJson() const {
  std::lock_guard<std::mutex> lock(stateMutex_);
  nlohmann::json queue = nlohmann::json::array();
  for (const auto id : queue_) queue.push_back(id);
  return {{"trackId", currentId_},
          {"playing", playing_.load()},
          {"paused", paused_.load()},
          {"positionMs", positionMsLocked()},
          {"durationMs", durationMs_},
          {"volume", volume_.load()},
          {"queue", queue},
          {"queueIndex", queueIndex_},
          {"audioMode", audio_.mode()},
          {"error", lastError_}};
}

// 音频回调：从环形缓冲取 frames 帧。返回实际取到的帧数。
// 数据不足时补静音，并只按实际取到的帧数推进 position。
// ringMutex_ 用来和 seek 时的 clear() 互斥；正常情况下几乎不会争用。
std::size_t Playback::fillAudio(float* dst, std::size_t frames) {
  const int channels = audio_.channels();
  if (channels <= 0) return 0;
  const std::size_t samples = frames * static_cast<std::size_t>(channels);
  std::size_t got = 0;
  {
    std::lock_guard<std::mutex> lock(ringMutex_);
    got = ring_.read(dst, samples);
  }
  if (got < samples) {
    std::memset(dst + got, 0, (samples - got) * sizeof(float));
  }
  const std::size_t gotFrames = got / static_cast<std::size_t>(channels);
  framesPlayed_.fetch_add(static_cast<int64_t>(gotFrames));
  return gotFrames;
}

// 解码线程主循环：负责整个播放队列。
// 每一轮：
//   1. 读取当前曲目 path（必要时从 DB 补）
//   2. decodeFile() 解码到 EOF
//   3. 等待环形缓冲排空（让最后一小段真正播完）
//   4. 自动切到下一首；队列结束则停止
// ActiveGuard 保证线程退出时 decoderActive_ 复位，resume() 可据此判断是否需要重启。
void Playback::decoderLoop() {
  struct ActiveGuard {
    std::atomic<bool>& flag;
    ~ActiveGuard() { flag.store(false); }
  } guard{decoderActive_};

  while (!decoderStop_.load() && !shuttingDown_.load()) {
    int64_t trackId = 0;
    std::string path;
    {
      std::lock_guard<std::mutex> lock(stateMutex_);
      if (queue_.empty() || queueIndex_ >= queue_.size()) {
        playing_.store(false);
        break;
      }
      trackId = queue_[queueIndex_];
      path = currentPath_;
    }

    if (path.empty()) {
      const auto track = db_.getTrack(trackId);
      if (!track) {
        setError("track vanished from library");
        playing_.store(false);
        break;
      }
      {
        std::lock_guard<std::mutex> lock(stateMutex_);
        currentPath_ = track->path;
        durationMs_ = track->durationMs;
      }
      path = track->path;
    }

    if (!decodeFile(path)) {
      if (decoderStop_.load() || shuttingDown_.load()) break;
      playing_.store(false);
      audio_.pause();
      break;
    }
    if (decoderStop_.load() || shuttingDown_.load()) break;

    // 解码到 EOF 后，环形缓冲里可能还有几百毫秒数据。
    // 等它被音频线程消费完再切歌，避免尾音被截断。
    for (;;) {
      if (decoderStop_.load() || shuttingDown_.load()) break;
      if (paused_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      std::size_t available = 0;
      {
        std::lock_guard<std::mutex> lock(ringMutex_);
        available = ring_.available();
      }
      if (available == 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (decoderStop_.load() || shuttingDown_.load()) break;

    // 决定下一首：队列末尾则停止（不自动循环，循环由前端/用户操作决定）。
    int64_t nextId = 0;
    bool hasNext = false;
    {
      std::lock_guard<std::mutex> lock(stateMutex_);
      if (queue_.empty()) {
        playing_.store(false);
        break;
      }
      if (queueIndex_ + 1 < queue_.size()) {
        nextId = queue_[queueIndex_ + 1];
        hasNext = true;
      } else {
        playing_.store(false);
        paused_.store(false);
      }
    }
    if (!hasNext) {
      audio_.pause();
      break;
    }

    const auto next = db_.getTrack(nextId);
    if (!next) {
      setError("next track vanished from library");
      playing_.store(false);
      audio_.pause();
      break;
    }
    {
      std::lock_guard<std::mutex> lock(stateMutex_);
      ++queueIndex_;
      currentId_ = nextId;
      currentPath_ = next->path;
      durationMs_ = next->durationMs;
    }
    framesPlayed_.store(0);
    seekBaseMs_.store(0);
    seekRequest_.store(-1);
  }
}

// 解码一个文件，直到 EOF / 停止 / 出错。
//
// FFmpeg 管线：
//   avformat_open_input -> avformat_find_stream_info -> av_find_best_stream
//   -> avcodec_open2 -> av_read_frame -> avcodec_send_packet
//   -> avcodec_receive_frame -> Swr 重采样为设备格式 -> 写环形缓冲
//
// 返回 true 表示正常到达 EOF；false 表示被停止或发生错误
//（错误信息已通过 setError 记录）。
bool Playback::decodeFile(const std::string& path) {
  AVFormatContext* fmt = nullptr;
  AVCodecContext* codec = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  SwrContext* swr = nullptr;
  bool swrReady = false;

  // FFmpeg 对象没有 RAII，统一在函数返回前手工释放。
  // 注意释放顺序：先释放依赖方（swr/frame/packet/codec），最后关闭输入。
  auto cleanup = [&]() {
    if (swr) swr_free(&swr);
    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);
    if (codec) avcodec_free_context(&codec);
    if (fmt) avformat_close_input(&fmt);
  };

  if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
    cleanup();
    if (!decoderStop_.load()) setError("cannot open audio file");
    return false;
  }
  if (avformat_find_stream_info(fmt, nullptr) < 0) {
    cleanup();
    if (!decoderStop_.load()) setError("cannot read stream info");
    return false;
  }
  const int streamIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (streamIndex < 0) {
    cleanup();
    if (!decoderStop_.load()) setError("no audio stream");
    return false;
  }

  const AVCodec* decoder =
      avcodec_find_decoder(fmt->streams[streamIndex]->codecpar->codec_id);
  if (!decoder) {
    cleanup();
    if (!decoderStop_.load()) setError("no decoder for stream");
    return false;
  }
  codec = avcodec_alloc_context3(decoder);
  if (!codec ||
      avcodec_parameters_to_context(codec, fmt->streams[streamIndex]->codecpar) < 0 ||
      avcodec_open2(codec, decoder, nullptr) < 0) {
    cleanup();
    if (!decoderStop_.load()) setError("cannot open decoder");
    return false;
  }

  packet = av_packet_alloc();
  frame = av_frame_alloc();
  if (!packet || !frame) {
    cleanup();
    if (!decoderStop_.load()) setError("out of memory");
    return false;
  }

  const int64_t formatDuration =
      fmt->duration > 0 ? fmt->duration * 1000 / AV_TIME_BASE : 0;
  if (formatDuration > 0) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (currentPath_ == path) durationMs_ = formatDuration;
  }

  // 输出目标始终是“音频设备的混音格式”：采样率/声道数来自 AudioOutput，
  // 样本格式固定为 AV_SAMPLE_FMT_FLT，后续 AudioOutput 再转成设备格式。
  const int outRate = audio_.sampleRate() > 0 ? audio_.sampleRate() : 48000;
  const int outChannels = audio_.channels() > 0 ? audio_.channels() : 2;
  AVChannelLayout outLayout{};
  av_channel_layout_default(&outLayout, outChannels);

  // 执行一次挂起的 seek 请求。
  // 步骤：换算时间基 -> av_seek_frame -> flush 解码器 -> 重置重采样器
  //       -> 清空环形缓冲 -> 若正在播放则恢复输出。
  auto doSeek = [&]() -> bool {
    const int64_t request = seekRequest_.exchange(-1);
    if (request < 0) return true;
    const AVRational ms{1, 1000};
    const int64_t timestamp =
        av_rescale_q(request, ms, fmt->streams[streamIndex]->time_base);
    if (av_seek_frame(fmt, streamIndex, timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
      setError("seek failed");
      return false;
    }
    avcodec_flush_buffers(codec);
    if (swrReady) {
      swr_close(swr);
      swr_init(swr);
    }
    {
      std::lock_guard<std::mutex> lock(ringMutex_);
      ring_.clear();
    }
    if (!paused_.load() && playing_.load()) audio_.resume();
    return true;
  };

  // 根据第一帧的输入格式初始化重采样器。
  // 输入可能是任意采样率/声道布局/样本格式，输出统一为 outRate/outChannels/FLT。
  auto initSwr = [&](AVFrame* f) -> bool {
    AVChannelLayout inLayout{};
    if (f->ch_layout.nb_channels > 0) {
      av_channel_layout_copy(&inLayout, &f->ch_layout);
    } else if (codec->ch_layout.nb_channels > 0) {
      av_channel_layout_copy(&inLayout, &codec->ch_layout);
    } else {
      av_channel_layout_default(&inLayout, 2);
    }
    const int inRate = f->sample_rate > 0 ? f->sample_rate : codec->sample_rate;
    const int ret = swr_alloc_set_opts2(
        &swr, &outLayout, AV_SAMPLE_FMT_FLT, outRate, &inLayout,
        static_cast<AVSampleFormat>(f->format), inRate, 0, nullptr);
    av_channel_layout_uninit(&inLayout);
    if (ret < 0) return false;
    if (swr_init(swr) < 0) return false;
    swrReady = true;
    return true;
  };

  // 把一帧解码结果重采样后写入环形缓冲。
  // 缓冲满时短暂休眠；期间检查停止/seek 请求，保证能及时响应。
  // 返回 false 表示被中断（停止或需要 seek），由调用方处理。
  auto writeFrame = [&](AVFrame* f) -> bool {
    if (!swrReady && !initSwr(f)) {
      setError("resampler init failed");
      return false;
    }
    const int inRate = f->sample_rate > 0 ? f->sample_rate : codec->sample_rate;
    const int outSamples = static_cast<int>(av_rescale_rnd(
        swr_get_delay(swr, inRate) + f->nb_samples, outRate, inRate, AV_ROUND_UP));
    if (outSamples <= 0) return true;

    std::vector<float> converted(static_cast<std::size_t>(outSamples) * outChannels);
    uint8_t* outPtr = reinterpret_cast<uint8_t*>(converted.data());
    const int got = swr_convert(swr, &outPtr, outSamples,
                                (const uint8_t**)f->extended_data, f->nb_samples);
    if (got < 0) {
      setError("resample failed");
      return false;
    }

    const std::size_t total = static_cast<std::size_t>(got) * outChannels;
    std::size_t offset = 0;
    while (offset < total) {
      if (decoderStop_.load()) return false;
      if (seekRequest_.load() >= 0) return false;
      std::size_t written = 0;
      {
        std::lock_guard<std::mutex> lock(ringMutex_);
        written = ring_.write(converted.data() + offset, total - offset);
      }
      if (written == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      } else {
        offset += written;
      }
    }
    return true;
  };

  // 把解码器里已解码但尚未取出的帧全部取出并写出。
  // AVERROR(EAGAIN) 表示暂时没有更多帧（正常）；AVERROR_EOF 表示结束。
  auto drain = [&]() -> bool {
    for (;;) {
      const int ret = avcodec_receive_frame(codec, frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
      if (ret < 0) {
        setError("decode failed");
        return false;
      }
      if (!writeFrame(frame)) return false;
    }
  };

  // 主读包循环：
  //   处理 seek -> av_read_frame -> send_packet -> drain（receive_frame + writeFrame）
  // send 遇到 EAGAIN 时先 drain 再重试；写缓冲被中断时检查是否由 seek 引起。
  bool failed = false;
  while (!decoderStop_.load() && !shuttingDown_.load()) {
    if (seekRequest_.load() >= 0 && !doSeek()) {
      failed = true;
      break;
    }
    const int ret = av_read_frame(fmt, packet);
    if (ret == AVERROR(EAGAIN)) continue;
    if (ret < 0) break;

    if (packet->stream_index == streamIndex) {
      int send = avcodec_send_packet(codec, packet);
      if (send == AVERROR(EAGAIN)) {
        if (!drain()) {
          failed = true;
          av_packet_unref(packet);
          break;
        }
        send = avcodec_send_packet(codec, packet);
      }
      if (send < 0 && send != AVERROR_EOF) {
        setError("send packet failed");
        failed = true;
        av_packet_unref(packet);
        break;
      }
      if (!drain()) {
        av_packet_unref(packet);
        if (seekRequest_.load() >= 0) {
          if (!doSeek()) {
            failed = true;
            break;
          }
          continue;
        }
        failed = true;
        break;
      }
    }
    av_packet_unref(packet);
    if (seekRequest_.load() >= 0 && !doSeek()) {
      failed = true;
      break;
    }
  }

  // 送入空包让解码器把内部缓存的尾帧吐出来（flush）。
  if (!failed && !decoderStop_.load() && !shuttingDown_.load()) {
    avcodec_send_packet(codec, nullptr);
    drain();
  }

  av_channel_layout_uninit(&outLayout);
  cleanup();
  return !failed && !decoderStop_.load() && !shuttingDown_.load();
}

}  // namespace nf
