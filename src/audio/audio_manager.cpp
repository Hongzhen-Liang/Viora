// ============================================================
// 音频模块实现
// ============================================================
#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <math.h>
#include <string.h>

#include "audio/audio_manager.h"
#include "config.h"

// ---- PCM 环形缓存（唤醒/打断前置音频） ----
static int16_t s_pcm[PCM_BUFFER_SIZE];
static int     s_pcm_head = 0;
static int     s_pcm_len  = 0;

// ---- 播放环形缓冲（WebSocket 回调线程写入，主循环读出） ----
static uint8_t *s_play_buf  = nullptr;
static uint32_t s_play_head = 0;
static uint32_t s_play_len  = 0;
static bool     s_tts_end_seen = false;
static bool     s_play_started = false;
static bool     s_play_rebuffering = false;
static bool     s_play_session_active = false;
static bool     s_play_write_in_flight = false;
static uint32_t s_last_play_write_ms = 0;
static uint32_t s_play_empty_since_ms = 0;
static uint32_t s_play_underruns = 0;
static uint32_t s_play_overflows = 0;
static int64_t  s_play_last_write_us = 0;
static uint32_t s_play_max_write_gap_us = 0;
static uint32_t s_play_late_writes = 0;
static portMUX_TYPE s_play_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_play_write_mutex = nullptr;
static TaskHandle_t s_play_task = nullptr;

// 独立播放任务会把多块 PCM 提前写入 I2S DMA。AEC 参考也必须按相同
// 时间顺序排队，不能只保存“最近写入”的一块，否则预填 DMA 后会错位。
static constexpr uint32_t PLAY_REFERENCE_CAPACITY = 8192;  // 512ms
static int16_t s_play_reference[PLAY_REFERENCE_CAPACITY] = {};
static uint32_t s_play_reference_head = 0;
static uint32_t s_play_reference_len = 0;

// ---- 播放音量（LLM operation: volume_up / volume_down）----
static float s_volume = VOLUME_DEFAULT;

// ---- 麦克风幅度诊断 ----
static uint16_t s_capture_rms = 0;

// ---- 全局单例 ----
AudioManager g_audio;

void AudioManager::setVolume(float vol) {
  if (vol < VOLUME_MIN) vol = VOLUME_MIN;
  if (vol > VOLUME_MAX) vol = VOLUME_MAX;
  s_volume = vol;
}

float AudioManager::getVolume() {
  return s_volume;
}

// ============================================================
// 共享 I2S 总线（全双工：INMP441 RX + MAX98357A TX）
//   INMP441 与 MAX98357A 共享 BCLK=GPIO5 / WS=GPIO6，因此必须用
//   单个 I2S 端口同时收发，不能像旧硬件那样分两个独立端口。
// ============================================================
bool AudioManager::initI2s() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
    .sample_rate = SR_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // INMP441 是 24bit，装在 32bit 帧里
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // 麦克风 L/R=GND 左声道；功放播左声道
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    // 12 × 256 帧 = 192ms DMA 余量；独立播放任务会持续填满它。
    .dma_buf_count = 12,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true,  // TX 无数据时自动补零，喇叭保持静音
    .fixed_mclk = 0,
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_BCLK_PIN,      // GPIO5（INMP441 SCK = MAX98357A BCLK）
    .ws_io_num = I2S_WS_PIN,         // GPIO6（INMP441 WS = MAX98357A LRC）
    .data_out_num = I2S_SPK_DATA_PIN,  // GPIO15 → MAX98357A DIN
    .data_in_num = I2S_MIC_DATA_PIN,   // GPIO7 ← INMP441 SD
  };
  esp_err_t err = i2s_driver_install(I2S_PORT, &cfg, 0, nullptr);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] I2S init failed: %s\n", esp_err_to_name(err));
    return false;
  }
  err = i2s_set_pin(I2S_PORT, &pins);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] I2S pin config failed: %s\n", esp_err_to_name(err));
    return false;
  }
  Serial.printf(
      "[AUDIO] 共享 I2S 全双工就绪: BCLK=%d WS=%d DIN=%d SD=%d @%dHz\n",
      I2S_BCLK_PIN, I2S_WS_PIN, I2S_SPK_DATA_PIN, I2S_MIC_DATA_PIN,
      SR_SAMPLE_RATE);
  return true;
}

bool AudioManager::begin() {
  if (!initI2s()) {
    return false;
  }

  s_play_buf = (uint8_t *)ps_malloc(PLAY_BUFFER_SIZE);
  if (s_play_buf) {
    Serial.printf("[WS] 播放缓冲 %d KB 已分配(PSRAM)\n", PLAY_BUFFER_SIZE / 1024);
  } else {
    Serial.println("[WS] 警告：播放缓冲分配失败");
  }

  s_play_write_mutex = xSemaphoreCreateMutex();
  if (s_play_buf && s_play_write_mutex) {
    const BaseType_t created = xTaskCreatePinnedToCore(
        [](void *) {
          for (;;) {
            g_audio.playDrain();
            // 有数据时 i2s_write 本身按 DMA 节奏阻塞；无数据时短暂休眠，
            // 避免轮询占满 CPU，同时保持低开播延迟。
            vTaskDelay(pdMS_TO_TICKS(2));
          }
        },
        "tts-play", 4096, nullptr, 3, &s_play_task, 1);
    if (created == pdPASS) {
      Serial.println("[I2S] 独立 TTS 播放任务已启动（DMA 余量约 192ms）");
    } else {
      s_play_task = nullptr;
      Serial.println("[I2S] 警告：播放任务创建失败，回退主循环喂数");
    }
  } else {
    Serial.println("[I2S] 警告：播放任务依赖初始化失败，回退主循环喂数");
  }
  return true;
}

// ============================================================
// 麦克风采集（共享全双工总线，ONLY_LEFT 单声道）
// ============================================================
int AudioManager::capture(int16_t *pcm, int max_frames, int16_t *peak) {
  static int32_t raw[1024];  // 512 帧 × 单声道 32bit（static 避免占 loopTask 栈）
  size_t bytes_read = 0;
  if (i2s_read(I2S_PORT, raw, max_frames * sizeof(int32_t), &bytes_read,
               portMAX_DELAY) != ESP_OK)
    return 0;
  int frames = bytes_read / sizeof(int32_t);  // ONLY_LEFT：每帧 = 1 个 32bit slot
  if (frames > max_frames) frames = max_frames;
  int32_t vol = 0;
  uint64_t square_sum = 0;
  for (int i = 0; i < frames; i++) {
    // 麦克风的 24-bit 二补码样本位于 32-bit I2S slot 高位。直接取高
    // 16 bit；旧实现右移 14 位等于额外放大 4 倍，真人近讲时大量样本
    // 被压到 +/-32767，削波后的谐波会显著拉低 KWS 分数。Log-Mel 在
    // 每个窗口内会做均值/方差归一化，不需要用数字增益换灵敏度。
    const int32_t sample = raw[i] >> 16;  // INMP441 L/R=GND → 左声道
    const int16_t l = static_cast<int16_t>(sample);
    pcm[i] = l;
    const int32_t al = sample < 0 ? -sample : sample;
    if (al > vol) vol = al;
    square_sum += static_cast<int64_t>(sample) * sample;
  }
  s_capture_rms = frames > 0
                      ? static_cast<uint16_t>(sqrt(static_cast<double>(square_sum) / frames))
                      : 0;
  if (peak) *peak = static_cast<int16_t>(vol > INT16_MAX ? INT16_MAX : vol);
  return frames;
}

uint16_t AudioManager::captureRms() { return s_capture_rms; }

// ============================================================
// 唤醒/打断前置音频环形缓存
// ============================================================
void AudioManager::ringPush(const int16_t *src, int n) {
  for (int i = 0; i < n; i++) {
    s_pcm[(s_pcm_head + s_pcm_len) % PCM_BUFFER_SIZE] = src[i];
    s_pcm_len++;
  }
  // 覆盖最旧数据，防止溢出
  if (s_pcm_len > PCM_BUFFER_SIZE) {
    int drop = s_pcm_len - PCM_BUFFER_SIZE;
    s_pcm_head = (s_pcm_head + drop) % PCM_BUFFER_SIZE;
    s_pcm_len = PCM_BUFFER_SIZE;
  }
}

bool AudioManager::ringTake(int16_t *dst, int n) {
  if (s_pcm_len < n) return false;
  for (int i = 0; i < n; i++) dst[i] = s_pcm[(s_pcm_head + i) % PCM_BUFFER_SIZE];
  s_pcm_head = (s_pcm_head + n) % PCM_BUFFER_SIZE;
  s_pcm_len -= n;
  return true;
}

int AudioManager::ringSize() { return s_pcm_len; }

void AudioManager::ringClear() {
  s_pcm_head = 0;
  s_pcm_len = 0;
}

// ============================================================
// TTS 播放缓冲（临界区保护）
// ============================================================
void AudioManager::playPush(const uint8_t *src, uint32_t n) {
  if (s_play_buf == nullptr || src == nullptr || n == 0) return;
  n -= n % sizeof(int16_t);
  if (n == 0) return;
  uint32_t dropped = 0;
  portENTER_CRITICAL(&s_play_mux);
  if (!s_play_session_active) {
    portEXIT_CRITICAL(&s_play_mux);
    return;
  }
  if (n >= PLAY_BUFFER_SIZE) {
    // 理论上 WS 帧已限制为 4096B；这里仍防御异常大帧。
    src += n - PLAY_BUFFER_SIZE;
    n = PLAY_BUFFER_SIZE;
    dropped = s_play_len;
    s_play_head = 0;
    s_play_len = 0;
  } else if (s_play_len + n > PLAY_BUFFER_SIZE) {
    // 缓冲区满：丢弃最旧数据，保留最新
    dropped = s_play_len + n - PLAY_BUFFER_SIZE;
    s_play_head = (s_play_head + dropped) % PLAY_BUFFER_SIZE;
    s_play_len -= dropped;
  }
  // PSRAM 逐字节写入会长时间占用 WebSocket 回调；拆成环尾/环头
  // 两次 memcpy，缩短收包临界区时间。
  const uint32_t tail = (s_play_head + s_play_len) % PLAY_BUFFER_SIZE;
  const uint32_t first = n < PLAY_BUFFER_SIZE - tail
                             ? n
                             : PLAY_BUFFER_SIZE - tail;
  memcpy(s_play_buf + tail, src, first);
  if (n > first) memcpy(s_play_buf, src + first, n - first);
  s_play_len += n;
  if (dropped > 0) ++s_play_overflows;
  portEXIT_CRITICAL(&s_play_mux);
  if (dropped > 0) {
    Serial.printf("[I2S] 播放缓冲溢出 #%lu：丢弃 %luB\n",
                  static_cast<unsigned long>(s_play_overflows),
                  static_cast<unsigned long>(dropped));
  }
}

uint32_t AudioManager::playBufferedBytes() {
  portENTER_CRITICAL(&s_play_mux);
  const uint32_t n = s_play_len;
  portEXIT_CRITICAL(&s_play_mux);
  return n;
}

void AudioManager::playDrain() {
  uint32_t n;
  bool active;
  bool end_seen;
  bool started;
  bool rebuffering;
  portENTER_CRITICAL(&s_play_mux);
  n = s_play_len;
  active = s_play_session_active;
  end_seen = s_tts_end_seen;
  started = s_play_started;
  rebuffering = s_play_rebuffering;
  portEXIT_CRITICAL(&s_play_mux);

  if (!active) return;

  const uint32_t now = millis();
  const uint32_t prebuffer_bytes =
      (SR_SAMPLE_RATE * 2U * PLAY_PREBUFFER_MS) / 1000U;
  const uint32_t rebuffer_bytes =
      (SR_SAMPLE_RATE * 2U * PLAY_REBUFFER_MS) / 1000U;

  // 应用缓冲短暂触底可能只是 WS 包恰好处在两次 loop 之间。
  // 连续一个播放帧仍为空才认定真实欠载。
  if (started && n == 0 && !end_seen) {
    bool underrun = false;
    uint32_t underrun_count = 0;
    portENTER_CRITICAL(&s_play_mux);
    if (s_play_empty_since_ms == 0) {
      s_play_empty_since_ms = now;
    } else if (now - s_play_empty_since_ms >= PLAY_UNDERRUN_GRACE_MS) {
      s_play_started = false;
      s_play_rebuffering = true;
      s_play_empty_since_ms = 0;
      underrun_count = ++s_play_underruns;
      underrun = true;
    }
    portEXIT_CRITICAL(&s_play_mux);
    if (underrun) {
      Serial.printf(
          "[I2S] TTS 播放欠载 #%lu，等待重缓冲 %dms\n",
          static_cast<unsigned long>(underrun_count), PLAY_REBUFFER_MS);
    }
    return;
  }
  if (n > 0) {
    portENTER_CRITICAL(&s_play_mux);
    s_play_empty_since_ms = 0;
    portEXIT_CRITICAL(&s_play_mux);
  }

  const uint32_t start_bytes =
      rebuffering ? rebuffer_bytes : prebuffer_bytes;
  if (!started && n > 0 && !end_seen && n < start_bytes) {
    // 尚未达到首播/重缓冲水位，暂不把抖动直接传给 I2S。
    return;
  }

  if (n > 0) {
    // 与打断时的 i2s_zero_dma_buffer 串行，确保旧 TTS 不会在清 DMA 后
    // 又写回一小块尾音。
    if (s_play_write_mutex &&
        xSemaphoreTake(s_play_write_mutex, portMAX_DELAY) != pdTRUE) {
      return;
    }

    // 每次写 512 样本(1024 字节 = 32ms)，与麦克风 32ms 一帧对齐，保持实时播放
    uint32_t chunk = 0;
    static uint8_t buf[1024];
    portENTER_CRITICAL(&s_play_mux);
    // 获取写锁后重新检查，处理并发打断或正常结束。
    if (s_play_session_active && s_play_len > 0) {
      if (!s_play_started) {
        started = false;
        rebuffering = s_play_rebuffering;
      }
      s_play_started = true;
      s_play_rebuffering = false;
      chunk = s_play_len < sizeof(buf) ? s_play_len : sizeof(buf);
      const uint32_t first = chunk < PLAY_BUFFER_SIZE - s_play_head
                                 ? chunk
                                 : PLAY_BUFFER_SIZE - s_play_head;
      memcpy(buf, s_play_buf + s_play_head, first);
      if (chunk > first) memcpy(buf + first, s_play_buf, chunk - first);
      s_play_head = (s_play_head + chunk) % PLAY_BUFFER_SIZE;
      s_play_len -= chunk;
      s_play_write_in_flight = true;
    }
    portEXIT_CRITICAL(&s_play_mux);

    if (chunk == 0) {
      if (s_play_write_mutex) xSemaphoreGive(s_play_write_mutex);
      return;
    }
    if (!started && !rebuffering) {
      Serial.printf("[I2S] TTS 开播缓冲就绪：%luB\n",
                    static_cast<unsigned long>(n));
    } else if (!started && rebuffering) {
      Serial.printf("[I2S] TTS 重缓冲完成：%luB\n",
                    static_cast<unsigned long>(n));
    }

    // 音量缩放（32ms 一小块，开销可忽略）
    if (s_volume != 1.0f) {
      int16_t *s = (int16_t *)buf;
      int samples = chunk / 2;
      for (int i = 0; i < samples; i++) {
        int32_t v = (int32_t)((float)s[i] * s_volume);
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        s[i] = (int16_t)v;
      }
    }
    const int64_t write_started_us = esp_timer_get_time();
    if (s_play_last_write_us > 0) {
      const uint32_t gap_us = static_cast<uint32_t>(
          write_started_us - s_play_last_write_us);
      portENTER_CRITICAL(&s_play_mux);
      if (gap_us > s_play_max_write_gap_us) s_play_max_write_gap_us = gap_us;
      if (gap_us > PLAY_I2S_LATE_WRITE_MS * 1000U) ++s_play_late_writes;
      portEXIT_CRITICAL(&s_play_mux);
    }
    s_play_last_write_us = write_started_us;

    // 32bit 帧：int16 样本左移 16 位放入 32bit slot 高位（MAX98357A
    // 取每帧前 16bit，等价于直接播放 int16 样本）
    static int32_t tx_buf[512];
    const int tx_samples = chunk / sizeof(int16_t);
    const int16_t *s16 = reinterpret_cast<const int16_t *>(buf);
    for (int i = 0; i < tx_samples; ++i) {
      tx_buf[i] = static_cast<int32_t>(s16[i]) << 16;
    }
    size_t written = 0;
    const esp_err_t err =
        i2s_write(I2S_PORT, tx_buf, tx_samples * sizeof(int32_t), &written,
                  portMAX_DELAY);

    portENTER_CRITICAL(&s_play_mux);
    if (err == ESP_OK && written > 0 && s_play_session_active) {
      const uint32_t reference_frames = written / sizeof(int32_t);
      if (s_play_reference_len + reference_frames > PLAY_REFERENCE_CAPACITY) {
        const uint32_t drop = s_play_reference_len + reference_frames -
                              PLAY_REFERENCE_CAPACITY;
        s_play_reference_head =
            (s_play_reference_head + drop) % PLAY_REFERENCE_CAPACITY;
        s_play_reference_len -= drop;
      }
      const uint32_t tail =
          (s_play_reference_head + s_play_reference_len) %
          PLAY_REFERENCE_CAPACITY;
      const uint32_t first =
          reference_frames < PLAY_REFERENCE_CAPACITY - tail
              ? reference_frames
              : PLAY_REFERENCE_CAPACITY - tail;
      memcpy(s_play_reference + tail, buf, first * sizeof(int16_t));
      if (reference_frames > first) {
        memcpy(s_play_reference, reinterpret_cast<int16_t *>(buf) + first,
               (reference_frames - first) * sizeof(int16_t));
      }
      s_play_reference_len += reference_frames;
      s_last_play_write_ms = millis();
    }
    s_play_write_in_flight = false;
    portEXIT_CRITICAL(&s_play_mux);

    if (s_play_write_mutex) xSemaphoreGive(s_play_write_mutex);
    if (err != ESP_OK || written != chunk) {
      Serial.printf("[I2S] 播放写入异常 err=%s written=%u/%u\n",
                    esp_err_to_name(err), static_cast<unsigned>(written),
                    static_cast<unsigned>(chunk));
    }
  }
}

void AudioManager::playReference(int16_t *dst, int frames) {
  if (dst == nullptr || frames <= 0) return;
  portENTER_CRITICAL(&s_play_mux);
  const uint32_t requested = static_cast<uint32_t>(frames);
  const uint32_t available =
      s_play_reference_len < requested ? s_play_reference_len : requested;
  const uint32_t first = available < PLAY_REFERENCE_CAPACITY - s_play_reference_head
                             ? available
                             : PLAY_REFERENCE_CAPACITY - s_play_reference_head;
  memcpy(dst, s_play_reference + s_play_reference_head,
         first * sizeof(int16_t));
  if (available > first) {
    memcpy(dst + first, s_play_reference,
           (available - first) * sizeof(int16_t));
  }
  s_play_reference_head =
      (s_play_reference_head + available) % PLAY_REFERENCE_CAPACITY;
  s_play_reference_len -= available;
  portEXIT_CRITICAL(&s_play_mux);
  for (int i = available; i < frames; ++i) dst[i] = 0;
}

bool AudioManager::playTaskRunning() { return s_play_task != nullptr; }

void AudioManager::playDiscard() {
  // 先阻止播放任务取下一块，再等待正在进行的单次写入结束。
  portENTER_CRITICAL(&s_play_mux);
  s_play_session_active = false;
  portEXIT_CRITICAL(&s_play_mux);
  if (s_play_write_mutex) xSemaphoreTake(s_play_write_mutex, portMAX_DELAY);
  portENTER_CRITICAL(&s_play_mux);
  s_play_head = 0;
  s_play_len = 0;
  s_tts_end_seen = false;
  s_play_started = false;
  s_play_rebuffering = false;
  s_play_empty_since_ms = 0;
  s_play_write_in_flight = false;
  s_play_reference_head = 0;
  s_play_reference_len = 0;
  portEXIT_CRITICAL(&s_play_mux);
  // 打断时不仅清应用缓冲，也立即清掉 I2S DMA 中尚未播放的尾音。
  i2s_zero_dma_buffer(I2S_PORT);
  if (s_play_write_mutex) xSemaphoreGive(s_play_write_mutex);
}

void AudioManager::markTtsStart() {
  portENTER_CRITICAL(&s_play_mux);
  s_tts_end_seen = false;
  s_play_started = false;
  s_play_rebuffering = false;
  s_play_session_active = true;
  s_play_write_in_flight = false;
  s_play_empty_since_ms = 0;
  s_play_underruns = 0;
  s_play_overflows = 0;
  s_play_last_write_us = 0;
  s_play_max_write_gap_us = 0;
  s_play_late_writes = 0;
  s_play_reference_head = 0;
  s_play_reference_len = 0;
  portEXIT_CRITICAL(&s_play_mux);
}

void AudioManager::markTtsEnd() {
  portENTER_CRITICAL(&s_play_mux);
  s_tts_end_seen = true;
  portEXIT_CRITICAL(&s_play_mux);
}

bool AudioManager::playbackFinished() {
  uint32_t n;
  uint32_t reference_frames;
  bool end_seen;
  bool started;
  bool write_in_flight;
  uint32_t last_write_ms;
  portENTER_CRITICAL(&s_play_mux);
  n = s_play_len;
  reference_frames = s_play_reference_len;
  end_seen = s_tts_end_seen;
  started = s_play_started;
  write_in_flight = s_play_write_in_flight;
  last_write_ms = s_last_play_write_ms;
  portEXIT_CRITICAL(&s_play_mux);

  if (!end_seen) return false;
  if (n == 0 && reference_frames == 0 && !write_in_flight) {
    // 参考队列与麦克风同速消费，清空意味着 DMA 时间轴也已基本走完；
    // 再留一个小尾量，避免末尾被下一轮聆听误识别。
    if (started && millis() - last_write_ms < PLAYBACK_DRAIN_MS) {
      return false;
    }

    uint32_t max_gap_us;
    uint32_t late_writes;
    uint32_t underruns;
    uint32_t overflows;
    portENTER_CRITICAL(&s_play_mux);
    // 获取快照后可能正好又写入，必须在锁内复核完成条件。
    if (!s_tts_end_seen || s_play_len != 0 ||
        s_play_reference_len != 0 || s_play_write_in_flight) {
      portEXIT_CRITICAL(&s_play_mux);
      return false;
    }
    max_gap_us = s_play_max_write_gap_us;
    late_writes = s_play_late_writes;
    underruns = s_play_underruns;
    overflows = s_play_overflows;
    s_tts_end_seen = false;
    s_play_started = false;
    s_play_rebuffering = false;
    s_play_session_active = false;
    s_play_empty_since_ms = 0;
    s_play_reference_head = 0;
    s_play_reference_len = 0;
    portEXIT_CRITICAL(&s_play_mux);
    Serial.printf(
        "[I2S] TTS 播放完成：最大写间隔=%.1fms 延迟写=%lu 欠载=%lu 溢出=%lu\n",
        max_gap_us / 1000.0f, static_cast<unsigned long>(late_writes),
        static_cast<unsigned long>(underruns),
        static_cast<unsigned long>(overflows));
    return true;
  }
  return false;
}
