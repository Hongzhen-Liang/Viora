// ============================================================
// 音频模块实现
// ============================================================
#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "audio.h"

// ---- PCM 环形缓存（喂 wakenet） ----
static int16_t s_pcm[PCM_BUFFER_SIZE];
static int     s_pcm_head = 0;
static int     s_pcm_len  = 0;

// ---- 播放环形缓冲（WebSocket 回调线程写入，主循环读出） ----
static uint8_t *s_play_buf  = nullptr;
static uint32_t s_play_head = 0;
static uint32_t s_play_len  = 0;
static bool     s_tts_end_seen = false;
static portMUX_TYPE s_play_mux = portMUX_INITIALIZER_UNLOCKED;

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
  Serial.printf("[I2S] 扬声器初始化完成, BCK=%d WS=%d DIN=%d @%dHz\n",
                SPK_BCK, SPK_WS, SPK_DIN, SR_SAMPLE_RATE);
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
  int16_t vol = 0;
  for (int i = 0; i < frames; i++) {
    int16_t l = (int16_t)(raw[2 * i] >> 14);      // 左声道（已确认数据在 L）
    pcm[i] = l;
    int16_t al = l < 0 ? -l : l;
    if (al > vol) vol = al;
  }
  if (peak) *peak = vol;
  return frames;
}

// ============================================================
// wakenet 环形缓存
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

void audio_play_drain() {
  uint32_t n;
  portENTER_CRITICAL(&s_play_mux);
  n = s_play_len;
  portEXIT_CRITICAL(&s_play_mux);

  if (n > 0) {
    // 每次写 512 样本(1024 字节 = 32ms)，与麦克风 32ms 一帧对齐，保持实时播放
    uint32_t chunk = n < 1024 ? n : 1024;
    static uint8_t buf[1024];
    portENTER_CRITICAL(&s_play_mux);
    for (uint32_t i = 0; i < chunk; i++) buf[i] = s_play_buf[(s_play_head + i) % PLAY_BUFFER_SIZE];
    s_play_head = (s_play_head + chunk) % PLAY_BUFFER_SIZE;
    s_play_len -= chunk;
    portEXIT_CRITICAL(&s_play_mux);
    size_t written = 0;
    i2s_write(SPK_I2S_PORT, buf, chunk, &written, portMAX_DELAY);
  }
}

void audio_play_discard() {
  portENTER_CRITICAL(&s_play_mux);
  s_play_head = 0;
  s_play_len = 0;
  portEXIT_CRITICAL(&s_play_mux);
  s_tts_end_seen = false;
}

void audio_mark_tts_start() {
  s_tts_end_seen = false;
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
    s_tts_end_seen = false;
    return true;
  }
  return false;
}
