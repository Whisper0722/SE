// =============================================================================
// audio_output.cpp - WASAPI 共享模式输出（带 null 降级）
//
// 初始化：COM(MTA) -> 默认渲染设备 -> IAudioClient -> 混音格式
//         -> 事件驱动初始化 -> IAudioRenderClient
// 运行时：音频线程等待事件，从 FillCallback 取 float 样本，
//         应用音量、转换设备格式（float32/int16），提交给 WASAPI。
//
// 所有 Windows/COM 类型都藏在 Impl 里，头文件不依赖 windows.h。
// =============================================================================

#include "audio_output.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <initguid.h>
#include <windows.h>
#include <combaseapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>

#include "util.hpp"

namespace nf {

// Windows/COM 资源的实际持有者。
struct AudioOutput::Impl {
  // ---- COM 对象（按创建顺序持有，releaseCom 逆序释放）----
  IMMDeviceEnumerator* enumerator = nullptr;
  IMMDevice* device = nullptr;
  IAudioClient* client = nullptr;
  IAudioRenderClient* render = nullptr;
  HANDLE event = nullptr;          // WASAPI 事件回调句柄
  WAVEFORMATEX* mix = nullptr;     // 设备混音格式（CoTaskMem 分配）
  UINT32 bufferFrames = 0;         // 设备缓冲区帧数

  // ---- 实际输出格式 ----
  int sampleRate = 48000;
  int channels = 2;
  bool floatFormat = true;   // true=float32，false=int16
  bool nullMode = true;      // 没有可用设备时走 null 输出
  bool comInitialized = false;
  bool comChangedMode = false;  // 当前线程已用其他 COM 模式初始化

  // ---- 音频线程状态 ----
  std::atomic<bool> paused{false};
  std::atomic<bool> running{false};
  bool threadStarted = false;
  std::thread thread;
  std::mutex threadMutex;    // 保护 thread 的启动/join

  // ---- 数据来源与输出 ----
  FillCallback fill;         // Playback 提供的取数回调
  std::atomic<float> volume{0.8f};
  std::vector<float> scratch;  // 回调写入的临时 float 缓冲
  std::string modeName = "null";

  bool initWasapi();
  bool detectFormat(const WAVEFORMATEX* wf);
  void threadMain();
  void releaseCom();
};

namespace {

// 判断设备混音格式是 float 还是 PCM。
// WAVE_FORMAT_EXTENSIBLE 时真正的格式在 SubFormat GUID 的 Data1：
//   3 = IEEE float，1 = PCM。
bool isFloatFormat(const WAVEFORMATEX* wf) {
  if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
  if (wf->wFormatTag == WAVE_FORMAT_PCM) return false;
  if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
    // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {00000003-...}
    // KSDATAFORMAT_SUBTYPE_PCM        = {00000001-...}
    if (ext->SubFormat.Data1 == 3) return true;
    if (ext->SubFormat.Data1 == 1) return false;
  }
  return false;
}

}  // namespace

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}
AudioOutput::~AudioOutput() { shutdown(); }

// 打开默认渲染设备并创建事件驱动的共享模式流。
// 任何一步失败都返回 false，由 initialize() 降级到 null 输出。
bool AudioOutput::Impl::initWasapi() {
  // COM 设备枚举器。
  HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                IID_IMMDeviceEnumerator,
                                reinterpret_cast<void**>(&enumerator));
  if (FAILED(hr)) return false;

  // 默认播放设备（eRender + eConsole）。
  hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  if (FAILED(hr)) return false;

  // 激活 IAudioClient。
  hr = device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                        reinterpret_cast<void**>(&client));
  if (FAILED(hr)) return false;

  // 共享模式必须使用设备混音格式，否则 Initialize 会失败。
  hr = client->GetMixFormat(&mix);
  if (FAILED(hr) || !mix) return false;
  if (!detectFormat(mix)) return false;

  // 20 ms 事件驱动缓冲区。AUDCLNT_STREAMFLAGS_EVENTCALLBACK 表示
  // 每消费完一块数据就向 event 发信号，而不是让音频线程轮询。
  const REFERENCE_TIME duration = 200000;
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                          AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration, 0, mix,
                          nullptr);
  if (FAILED(hr)) return false;

  // 自动复位事件（第二个参数 FALSE）。
  event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event) return false;
  hr = client->SetEventHandle(event);
  if (FAILED(hr)) return false;

  // 获取渲染客户端，用于 GetBuffer/ReleaseBuffer。
  hr = client->GetService(IID_IAudioRenderClient,
                          reinterpret_cast<void**>(&render));
  if (FAILED(hr)) return false;

  hr = client->GetBufferSize(&bufferFrames);
  if (FAILED(hr)) return false;

  sampleRate = static_cast<int>(mix->nSamplesPerSec);
  channels = static_cast<int>(mix->nChannels);
  return sampleRate > 0 && channels > 0;
}

// 只支持 float32 和 int16 两种混音格式；其他格式降级 null。
// floatFormat 会决定 threadMain() 里的转换分支。
bool AudioOutput::Impl::detectFormat(const WAVEFORMATEX* wf) {
  const bool isFloat = isFloatFormat(wf);
  if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
    if (ext->SubFormat.Data1 != 1 && ext->SubFormat.Data1 != 3) return false;
  } else if (wf->wFormatTag != WAVE_FORMAT_IEEE_FLOAT &&
             wf->wFormatTag != WAVE_FORMAT_PCM) {
    return false;
  }
  if (isFloat && wf->wBitsPerSample != 32) return false;
  if (!isFloat && wf->wBitsPerSample != 16) return false;
  floatFormat = isFloat;
  return true;
}

// 初始化输出。
//   * forceNull=true：跳过 WASAPI，直接使用 48k/2ch 的 null 输出
//   * 否则尝试 WASAPI；失败则记录日志并降级
// 无论哪种模式都返回 true，让播放引擎仍然可以工作。
bool AudioOutput::initialize(bool forceNull) {
  Impl& d = *impl_;
  const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  d.comInitialized = SUCCEEDED(hr);
  d.comChangedMode = (hr == RPC_E_CHANGED_MODE);

  d.nullMode = true;
  d.modeName = "null";
  if (!forceNull && (d.comInitialized || d.comChangedMode)) {
    if (d.initWasapi()) {
      d.nullMode = false;
      d.modeName = "wasapi";
      logMsg("audio: WASAPI shared mode " + std::to_string(d.sampleRate) + " Hz, " +
             std::to_string(d.channels) + " ch, " +
             (d.floatFormat ? "float32" : "int16"));
    } else {
      logMsg("audio: no usable render device, falling back to null output");
      d.releaseCom();
    }
  } else if (forceNull) {
    logMsg("audio: null output requested");
  } else {
    logMsg("audio: COM init failed, using null output");
  }

  if (d.nullMode) {
    d.sampleRate = 48000;
    d.channels = 2;
    d.floatFormat = true;
  }

  const size_t maxFrames = std::max<UINT32>(d.bufferFrames, 480);
  d.scratch.assign(maxFrames * static_cast<size_t>(d.channels), 0.0f);
  return true;
}

// 设置取数回调；必须在 start() 之前设置。
void AudioOutput::setFill(FillCallback callback) { impl_->fill = std::move(callback); }

// 软件音量，限制在 [0,1]。音频回调读取它并乘到样本上。
void AudioOutput::setVolume(float volume) {
  impl_->volume.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
}

int AudioOutput::sampleRate() const { return impl_->sampleRate; }
int AudioOutput::channels() const { return impl_->channels; }
std::string AudioOutput::mode() const { return impl_->modeName; }
bool AudioOutput::isNull() const { return impl_->nullMode; }

// 启动输出线程和 WASAPI 客户端。
// 重复调用是安全的：线程已存在时只重启客户端。
bool AudioOutput::start() {
  Impl& d = *impl_;
  std::lock_guard<std::mutex> lock(d.threadMutex);
  if (!d.threadStarted) {
    d.running.store(true);
    d.paused.store(false);
    d.threadStarted = true;
    d.thread = std::thread([this] { impl_->threadMain(); });
  }
  if (d.nullMode) {
    d.running.store(true);
    d.paused.store(false);
    return true;
  }
  if (!d.client) return false;
  if (FAILED(d.client->Start())) return false;
  d.running.store(true);
  d.paused.store(false);
  return true;
}

// 暂停输出：WASAPI 停止客户端（不再触发事件）；
// null 模式只置 paused 标志，线程会空转等待。
void AudioOutput::pause() {
  Impl& d = *impl_;
  d.paused.store(true);
  if (!d.nullMode && d.client) d.client->Stop();
}

// 恢复输出。线程尚未启动时先 start()。
void AudioOutput::resume() {
  Impl& d = *impl_;
  d.paused.store(false);
  if (d.nullMode) {
    if (!d.threadStarted) start();
    return;
  }
  if (!d.client) return;
  if (!d.threadStarted) {
    start();
    return;
  }
  d.client->Start();
}

// 停止并 join 音频线程。SetEvent 用来唤醒正在等待的 WASAPI 线程。
void AudioOutput::stop() {
  Impl& d = *impl_;
  d.running.store(false);
  if (d.event) SetEvent(d.event);
  {
    std::lock_guard<std::mutex> lock(d.threadMutex);
    if (d.thread.joinable()) d.thread.join();
    d.threadStarted = false;
  }
  if (!d.nullMode && d.client) d.client->Stop();
  d.paused.store(true);
}

// 音频线程主循环。
// WASAPI 模式：等待事件 -> 查询可写帧数 -> GetBuffer -> 调 FillCallback
//             -> 应用音量并转换为设备格式 -> ReleaseBuffer
// null 模式：每 10ms 消费 480 帧（48kHz 下的 10ms），只推进时间，不出声。
// 注意：这个线程需要尽量实时；不要在回调里做长时间阻塞操作。
void AudioOutput::Impl::threadMain() {
  // 每个线程使用 COM 前都要自己初始化；MTA 与主线程保持一致。
  const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool uninit = SUCCEEDED(hr);

  if (nullMode) {
    // 480 帧 @48kHz = 10ms。用固定节奏模拟声卡消费。
    constexpr std::size_t kChunkFrames = 480;
    std::vector<float> chunk(kChunkFrames * static_cast<std::size_t>(channels), 0.0f);
    while (running.load(std::memory_order_acquire)) {
      if (paused.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      std::fill(chunk.begin(), chunk.end(), 0.0f);
      if (fill) fill(chunk.data(), kChunkFrames);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  } else {
    // WASAPI 事件循环。200ms 超时只是为了让线程能定期检查 running。
    while (running.load(std::memory_order_acquire)) {
      const DWORD wait = WaitForSingleObject(event, 200);
      if (!running.load(std::memory_order_acquire)) break;
      if (wait != WAIT_OBJECT_0) continue;

      // padding 是设备里尚未播放的帧数；剩余空间就是这次可以写的帧数。
      UINT32 padding = 0;
      if (FAILED(client->GetCurrentPadding(&padding))) continue;
      const UINT32 available = bufferFrames - padding;
      if (available == 0) continue;

      BYTE* data = nullptr;
      if (FAILED(render->GetBuffer(available, &data)) || !data) continue;

      // 先清零，FillCallback 只覆盖它实际提供的部分；不足的样本保持静音。
      const std::size_t samples = static_cast<std::size_t>(available) * channels;
      if (scratch.size() < samples) scratch.resize(samples, 0.0f);
      std::fill(scratch.begin(), scratch.begin() + samples, 0.0f);
      if (fill) fill(scratch.data(), available);

      // 软件音量 + 防止削波，再按设备格式提交。
      const float gain = volume.load(std::memory_order_relaxed);
      if (floatFormat) {
        auto* out = reinterpret_cast<float*>(data);
        for (std::size_t i = 0; i < samples; ++i) {
          out[i] = std::clamp(scratch[i] * gain, -1.0f, 1.0f);
        }
      } else {
        auto* out = reinterpret_cast<std::int16_t*>(data);
        for (std::size_t i = 0; i < samples; ++i) {
          const float v = std::clamp(scratch[i] * gain, -1.0f, 1.0f);
          out[i] = static_cast<std::int16_t>(v * 32767.0f);
        }
      }
      render->ReleaseBuffer(available, 0);
    }
  }

  if (uninit) CoUninitialize();
}

// 逆序释放所有 COM/系统资源。
// 调用前应已 stop()，否则音频线程可能仍在使用 client/render。
void AudioOutput::Impl::releaseCom() {
  if (render) {
    render->Release();
    render = nullptr;
  }
  if (client) {
    client->Stop();
    client->Release();
    client = nullptr;
  }
  if (device) {
    device->Release();
    device = nullptr;
  }
  if (enumerator) {
    enumerator->Release();
    enumerator = nullptr;
  }
  if (mix) {
    CoTaskMemFree(mix);
    mix = nullptr;
  }
  if (event) {
    CloseHandle(event);
    event = nullptr;
  }
  bufferFrames = 0;
}

// 完整关闭：停止线程 -> 释放 COM -> 若本线程初始化过 COM 则 CoUninitialize。
void AudioOutput::shutdown() {
  Impl& d = *impl_;
  stop();
  d.releaseCom();
  if (d.comInitialized) {
    CoUninitialize();
    d.comInitialized = false;
  }
}

}  // namespace nf
