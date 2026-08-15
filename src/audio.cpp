// ============================================================
// 音频模块实现
// ============================================================
#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <math.h>

#include "config.h"
#include "audio.h"

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
static uint32_t s_last_play_write_ms = 0;
static portMUX_TYPE s_play_mux = portMUX_INITIALIZER_UNLOCKED;

// 上一块实际写入功放的音频。主循环先采麦、再写下一块扬声器数据，
// 因而这个块正好可作为本次麦克风帧的 AEC 参考。
static int16_t s_play_reference[512] = {};
static int     s_play_reference_frames = 0;

// ---- 播放音量（LLM operation: volume_up / volume_down）----
static float s_volume = 1.0f;

// ---- 麦克风幅度诊断 ----
static uint16_t s_capture_rms = 0;

void audio_set_volume(float vol) {
  if (vol < VOLUME_MIN) vol = VOLUME_MIN;
  if (vol > VOLUME_MAX) vol = VOLUME_MAX;
  s_volume = vol;
}

float audio_get_volume() {
  return s_volume;
}

// ============================================================
// 麦克风 I2S（RX，32bit 帧装 24bit 数据，双声道诊断读左声道）
// ============================================================
static void init_i2s_mic() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SR_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // INMP441 是 24bit，装在 32bit 帧里
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,  // 读双声道，取左声道
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0,
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD,
  };
  ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &cfg, 0, nullptr));
  ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pins));
  Serial.printf("[I2S] 初始化完成, SCK=%d WS=%d SD=%d @%dHz\n",
                I2S_SCK, I2S_WS, I2S_SD, SR_SAMPLE_RATE);
}

// ============================================================
// 扬声器 I2S（TX，输出 TTS 音频到 MAX98357）
// ============================================================
static void init_i2s_speaker() {
  // MAX98357A 的 SD_MODE 悬空时状态不可靠。初始化期间先保持关断，
  // 等 BCLK/LRCLK/DIN 均配置完成后再拉高，启用与当前输出匹配的左声道。
  pinMode(SPK_SD, OUTPUT);
  digitalWrite(SPK_SD, LOW);

  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SR_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,   // 单声道
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0,
  };
  i2s_pin_config_t pins = {
    .bck_io_num = SPK_BCK,
    .ws_io_num = SPK_WS,
    .data_out_num = SPK_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE,
  };
  ESP_ERROR_CHECK(i2s_driver_install(SPK_I2S_PORT, &cfg, 0, nullptr));
  ESP_ERROR_CHECK(i2s_set_pin(SPK_I2S_PORT, &pins));
  digitalWrite(SPK_SD, HIGH);
  Serial.printf(
      "[I2S] 扬声器初始化完成, BCK=%d WS=%d DIN=%d SD=%d(HIGH) @%dHz\n",
      SPK_BCK, SPK_WS, SPK_DIN, SPK_SD, SR_SAMPLE_RATE);
}

void audio_init() {
  init_i2s_mic();
  init_i2s_speaker();

  s_play_buf = (uint8_t *)ps_malloc(PLAY_BUFFER_SIZE);
  if (s_play_buf) {
    Serial.printf("[WS] 播放缓冲 %d KB 已分配(PSRAM)\n", PLAY_BUFFER_SIZE / 1024);
  } else {
    Serial.println("[WS] 警告：播放缓冲分配失败");
  }
}

// ============================================================
// 麦克风采集
// ============================================================
int audio_capture(int16_t *pcm, int max_frames, int16_t *peak) {
  static int32_t raw[1024];   // 512 帧 × 2 声道（static 避免占 loopTask 栈）
  size_t bytes_read = 0;
  if (i2s_read(I2S_PORT, raw, sizeof(raw), &bytes_read, portMAX_DELAY) != ESP_OK)
    return 0;
  int frames = bytes_read / 8;   // 每帧 = L(4B) + R(4B)
  if (frames > max_frames) frames = max_frames;
  int32_t vol = 0;
  uint64_t square_sum = 0;
  for (int i = 0; i < frames; i++) {
    // 麦克风的 24-bit 二补码样本位于 32-bit I2S slot 高位。右移 14 位
    // 保留原有的约 4x 数字增益，但必须显式饱和；直接强转 int16 会在
    // 大声说话或背景音乐较响时回绕，产生比普通削波更严重的频谱失真。
    int32_t sample = raw[2 * i] >> 14;             // 左声道（已确认数据在 L）
    if (sample > INT16_MAX) {
      sample = INT16_MAX;
    } else if (sample < INT16_MIN) {
      sample = INT16_MIN;
    }
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

uint16_t audio_capture_rms() { return s_capture_rms; }

// ============================================================
// 唤醒/打断前置音频环形缓存
// ============================================================
void audio_ring_push(const int16_t *src, int n) {
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

bool audio_ring_take(int16_t *dst, int n) {
  if (s_pcm_len < n) return false;
  for (int i = 0; i < n; i++) dst[i] = s_pcm[(s_pcm_head + i) % PCM_BUFFER_SIZE];
  s_pcm_head = (s_pcm_head + n) % PCM_BUFFER_SIZE;
  s_pcm_len -= n;
  return true;
}

int audio_ring_size() { return s_pcm_len; }

void audio_ring_clear() {
  s_pcm_head = 0;
  s_pcm_len = 0;
}

// ============================================================
// TTS 播放缓冲（临界区保护）
// ============================================================
void audio_play_push(const uint8_t *src, uint32_t n) {
  if (s_play_buf == nullptr) return;
  portENTER_CRITICAL(&s_play_mux);
  if (s_play_len + n > PLAY_BUFFER_SIZE) {
    // 缓冲区满：丢弃最旧数据，保留最新
    uint32_t drop = s_play_len + n - PLAY_BUFFER_SIZE;
    s_play_head = (s_play_head + drop) % PLAY_BUFFER_SIZE;
    s_play_len -= drop;
  }
  for (uint32_t i = 0; i < n; i++) {
    s_play_buf[(s_play_head + s_play_len) % PLAY_BUFFER_SIZE] = src[i];
    s_play_len++;
  }
  portEXIT_CRITICAL(&s_play_mux);
}

uint32_t audio_play_buffered_bytes() {
  portENTER_CRITICAL(&s_play_mux);
  const uint32_t n = s_play_len;
  portEXIT_CRITICAL(&s_play_mux);
  return n;
}

void audio_play_drain() {
  uint32_t n;
  portENTER_CRITICAL(&s_play_mux);
  n = s_play_len;
  portEXIT_CRITICAL(&s_play_mux);

  const uint32_t prebuffer_bytes =
      (SR_SAMPLE_RATE * 2U * PLAY_PREBUFFER_MS) / 1000U;
  if (!s_play_started && n > 0 && !s_tts_end_seen && n < prebuffer_bytes) {
    // 流式 TTS 刚开始时先积累约 128ms，换取无卡顿的连续播放。
    s_play_reference_frames = 0;
    return;
  }

  if (n > 0) {
    s_play_started = true;
    // 每次写 512 样本(1024 字节 = 32ms)，与麦克风 32ms 一帧对齐，保持实时播放
    uint32_t chunk = n < 1024 ? n : 1024;
    static uint8_t buf[1024];
    portENTER_CRITICAL(&s_play_mux);
    for (uint32_t i = 0; i < chunk; i++) buf[i] = s_play_buf[(s_play_head + i) % PLAY_BUFFER_SIZE];
    s_play_head = (s_play_head + chunk) % PLAY_BUFFER_SIZE;
    s_play_len -= chunk;
    portEXIT_CRITICAL(&s_play_mux);
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
    const int reference_frames = chunk / 2;
    for (int i = 0; i < reference_frames; ++i) {
      s_play_reference[i] = reinterpret_cast<int16_t *>(buf)[i];
    }
    s_play_reference_frames = reference_frames;
    size_t written = 0;
    i2s_write(SPK_I2S_PORT, buf, chunk, &written, portMAX_DELAY);
    s_last_play_write_ms = millis();
  } else {
    // 上一块已经覆盖了刚完成的采音窗口；下一窗口应使用静音参考。
    if (!s_tts_end_seen || millis() - s_last_play_write_ms >= PLAYBACK_DRAIN_MS) {
      s_play_reference_frames = 0;
    }
  }
}

void audio_play_reference(int16_t *dst, int frames) {
  if (dst == nullptr || frames <= 0) return;
  const int available = s_play_reference_frames < frames
                            ? s_play_reference_frames
                            : frames;
  for (int i = 0; i < available; ++i) dst[i] = s_play_reference[i];
  for (int i = available; i < frames; ++i) dst[i] = 0;
}

void audio_play_discard() {
  portENTER_CRITICAL(&s_play_mux);
  s_play_head = 0;
  s_play_len = 0;
  portEXIT_CRITICAL(&s_play_mux);
  s_tts_end_seen = false;
  s_play_started = false;
  s_play_reference_frames = 0;
  // 打断时不仅清应用缓冲，也立即清掉 I2S DMA 中尚未播放的尾音。
  i2s_zero_dma_buffer(SPK_I2S_PORT);
}

void audio_mark_tts_start() {
  s_tts_end_seen = false;
  s_play_started = false;
  s_play_reference_frames = 0;
}

void audio_mark_tts_end() {
  s_tts_end_seen = true;
}

bool audio_playback_finished() {
  if (!s_tts_end_seen) return false;
  uint32_t n;
  portENTER_CRITICAL(&s_play_mux);
  n = s_play_len;
  portEXIT_CRITICAL(&s_play_mux);
  if (n == 0) {
    // 应用环形缓冲清空不等于 DMA 已物理播完；非阻塞地等最后一小块
    // 离开功放，再开放下一轮麦克风，减少尾音被识别成用户话语。
    if (s_play_started &&
        millis() - s_last_play_write_ms < PLAYBACK_DRAIN_MS) {
      return false;
    }
    s_tts_end_seen = false;
    return true;
  }
  return false;
}
