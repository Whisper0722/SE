#pragma once

// =============================================================================
// audio_output.hpp - 音频输出抽象层
//
// 默认使用 Windows WASAPI 共享模式（事件驱动）：
//   * 打开默认播放设备，按设备混音格式创建 IAudioClient
//   * 独立音频线程等待事件，从 FillCallback 拉取 float 样本
//   * 根据设备格式转换为 float32 或 int16 后提交
//
// 如果 WASAPI 初始化失败（没有声卡/COM 失败），自动降级为 "null" 输出：
// 它按真实时间节奏消费样本并推进播放位置，便于无设备环境做测试。
// =============================================================================

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace nf {

class AudioOutput {
 public:
  // FillCallback 由 Playback 提供：把最多 frames 帧的交错 float 样本写进 dst，
  // 返回实际填充的帧数；不足部分由 AudioOutput 补静音。
  using FillCallback = std::function<std::size_t(float*, std::size_t)>;

  AudioOutput();
  ~AudioOutput();

  AudioOutput(const AudioOutput&) = delete;
  AudioOutput& operator=(const AudioOutput&) = delete;

  // 初始化 COM/WASAPI。forceNull=true 时跳过设备直接使用 null 输出（测试用）。
  bool initialize(bool forceNull = false);
  void setFill(FillCallback callback);
  void setVolume(float volume);  // 0.0 ~ 1.0，在音频回调里做软件增益

  int sampleRate() const;        // 实际输出采样率（WASAPI 混音格式）
  int channels() const;          // 实际输出声道数
  std::string mode() const;      // "wasapi" 或 "null"
  bool isNull() const;

  bool start();                  // 启动客户端/线程；可重复调用
  void pause();                  // 暂停输出（进度冻结）
  void resume();                 // 恢复输出
  void stop();                   // 停止并 join 音频线程
  void shutdown();               // 释放 COM 资源，析构时调用

 private:
  struct Impl;                   // Windows 类型藏在 .cpp 里，头文件保持干净
  std::unique_ptr<Impl> impl_;
};

}  // namespace nf
