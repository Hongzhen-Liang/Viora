#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "model_path.h"

// ============================================================
// 引脚定义（MSM3526 / INMP441，I2S 数字 MEMS 麦克风）
// 模块两排对接 ESP32 左排针连续引脚：
//   一排 [SD][VDD][GND] → GPIO12 / GPIO13(软件3.3V) / 真实GND
//   另一排 [L/R][WS][SCK] → GPIO17(拉低=左声道) / GPIO16 / GPIO15
// 均避开 USB(19/20)、八线PSRAM(26-37)、strapping(0/3/45/46)，且非 ADC1
// ============================================================
#define I2S_SCK  15
#define I2S_WS   16
#define I2S_SD   12
#define MIC_VDD  13   // 输出高电平 ≈3.3V，给麦克风 VDD 供电（~1.4mA，安全）
#define MIC_LR   17   // 输出低电平 → 左声道（模块 L/R 接这里）
#define I2S_PORT I2S_NUM_0

// 采样率（ESP-SR 固定要求 16kHz）
#define SR_SAMPLE_RATE 16000

// ============================================================
// WiFi 配置（连接 Mac 服务器所在局域网）
// ============================================================
#define WIFI_SSID "CHANGED-WIFI-SSID"
#define WIFI_PASS "CHANGED-WIFI-PASSWORD"

// ============================================================
// Mac 服务器（WebSocket）配置
// ============================================================
#define SERVER_HOST "CHANGED-SERVER-IP"   // TODO: 改成你 Mac 的局域网 IP
#define SERVER_PORT 8765
#define SERVER_PATH "/ws"

// ============================================================
// 扬声器（MAX98357 I2S 功放，输出 TTS 音频）
// 避开 USB(19/20)、PSRAM(26-37)、strapping(0/3/45/46)、麦克风(12/13/15/16/17)
// ============================================================
#define SPK_BCK      4
#define SPK_WS       5
#define SPK_DIN      6
#define SPK_I2S_PORT I2S_NUM_1

// 录音 VAD 参数（能量断句，语音阈值会在上电时自动校准）
#define VOICE_THRESHOLD_DEFAULT 400   // 校准前的兜底阈值
#define VOICE_THRESHOLD_MIN     500   // 自适应阈值下限（高于环境噪声尖峰~265）
#define VOICE_THRESHOLD_MAX     2500  // 自适应阈值上限
#define SILENCE_MS      5000   // 静音这么久才结束录音（聊天场景给足停顿时间）
#define MIN_REC_MS      800    // 至少录这么久
#define MAX_REC_MS      30000  // 最多录这么久（一次可连说 30 秒）

// 连续对话参数
#define CONV_TIMEOUT_MS 20000  // 连续对话中，这么久没人说话就退出（回到待唤醒）
#define GUARD_MS        400    // 播放结束后静置一小段，避免扬声器余音误触发

// 唤醒词 / 命令词模型名（必须与烧入 model 分区的模型一致）
static const char *WAKE_WORD_MODEL = "wn9_nihaoxiaozhi";  // 你好小智
static const char *COMMAND_MODEL   = "mn5q8_cn";          // 中文命令词
static const int   MN_COMMAND_WINDOW_MS = 2600;           // 唤醒后识别命令的窗口时长

// 环形缓存（保证喂给 wakenet / multinet 的音频流连续、各自取自己的帧长）
#define PCM_BUFFER_SIZE 4096
static int16_t s_pcm[PCM_BUFFER_SIZE];
static int     s_pcm_head = 0;
static int     s_pcm_len  = 0;

static const esp_wn_iface_t *s_wn = nullptr;
static model_iface_data_t   *s_wn_handle = nullptr;
static const esp_mn_iface_t *s_mn = nullptr;
static model_iface_data_t   *s_mn_handle = nullptr;
static bool s_woken = false;

// WiFi 重连计时
static uint32_t s_wifi_retry_ms = 0;
// WiFi 已连接标记（用于在连上时打印一次 IP）
static bool s_wifi_online = false;

// ============================================================
// WebSocket / 录音 / 播放 状态
// ============================================================
static WebSocketsClient s_ws;
static bool s_ws_connected = false;

static uint32_t s_rec_start_ms  = 0;
static uint32_t s_last_voice_ms = 0;
static uint16_t s_voice_threshold = VOICE_THRESHOLD_DEFAULT;  // 运行时语音阈值（持续自适应）
static uint32_t s_voice_frames = 0;   // 录音期间检测到"有声音"的帧数（诊断用）
static int32_t  s_rec_max_vol  = 0;   // 录音期间观测到的最大音量（诊断用）

// 持续噪声估计（最小统计法，只在空闲时更新）
#define NOISE_HIST_LEN 64   // 64 帧 × 32ms ≈ 2 秒
static int32_t s_noise_hist[NOISE_HIST_LEN];
static int     s_noise_idx   = 0;
static int     s_noise_count = 0;
static bool    s_vad_ready   = false;

// 对话状态机：IDLE(待唤醒) → LISTENING(聆听) → PROCESSING(等服务器) → PLAYING(播放)
enum ConvState { ST_IDLE, ST_LISTENING, ST_PROCESSING, ST_PLAYING };
static ConvState s_state = ST_IDLE;
static bool      s_speech_started  = false;  // 当前聆听窗口内是否已开始说话
static uint32_t  s_listen_start_ms = 0;      // 当前聆听窗口开始时间

static bool s_playing      = false;
static bool s_tts_end_seen = false;

// 播放环形缓冲（放 PSRAM：TTS 回复最长约 10s × 32KB/s ≈ 320KB）
#define PLAY_BUFFER_SIZE (512 * 1024)
static uint8_t *s_play_buf  = nullptr;
static uint32_t s_play_head = 0;
static uint32_t s_play_len  = 0;
static portMUX_TYPE s_play_mux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================
// I2S 初始化
// ============================================================
static void init_i2s() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SR_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // INMP441 是 24bit，装在 32bit 帧里
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,  // 诊断阶段读双声道，判断麦克风在左还是右
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
// 扬声器 I2S（输出 TTS 音频到 MAX98357）
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

// ============================================================
// 持续自适应 VAD：空闲时用最小统计法估计环境噪声，动态更新阈值
// ============================================================
static void vad_update(int32_t frame_peak) {
  s_noise_hist[s_noise_idx] = frame_peak;
  s_noise_idx = (s_noise_idx + 1) % NOISE_HIST_LEN;
  if (s_noise_count < NOISE_HIST_LEN) s_noise_count++;

  if (s_noise_count < NOISE_HIST_LEN) return;  // 还没采满 2 秒

  static int tick = 0;
  if (++tick < 8) return;   // 每 8 帧（约 250ms）重估一次，减少开销
  tick = 0;

  // 取 2 秒窗口内最小帧峰值作为噪声底（抗突发噪声，也抗开机时说话）
  int32_t mn = s_noise_hist[0];
  for (int i = 1; i < NOISE_HIST_LEN; i++) {
    if (s_noise_hist[i] < mn) mn = s_noise_hist[i];
  }

  int32_t t = mn * 3;   // 阈值 = 噪声底 × 3
  if (t < VOICE_THRESHOLD_MIN) t = VOICE_THRESHOLD_MIN;
  if (t > VOICE_THRESHOLD_MAX) t = VOICE_THRESHOLD_MAX;
  s_voice_threshold = (uint16_t)t;

  if (!s_vad_ready) {
    s_vad_ready = true;
    Serial.printf("[VAD] 就绪：噪声=%d → 阈值=%d（持续自适应中）\n",
                  (int)mn, (int)s_voice_threshold);
  }
}

static void enter_listening();  // 前向声明：play_drain 播放完成后自动进入下一轮聆听

// ============================================================
// 播放缓冲（WebSocket 回调线程 → 主循环，临界区保护）
// ============================================================
static void play_push(const uint8_t *src, uint32_t n) {
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

static void play_drain() {
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
  } else if (s_tts_end_seen) {
    s_playing = false;
    s_tts_end_seen = false;
    // 播放完成，自动进入下一轮聆听（连续对话，无需再喊唤醒词）
    enter_listening();
  }
}

// ============================================================
// 进入聆听状态（发 audio_start，开始收用户语音）
// ============================================================
static void enter_listening() {
  if (!s_ws_connected) {
    s_state = ST_IDLE;
    Serial.println(">>> 服务器未连接，回到待唤醒");
    return;
  }
  s_state = ST_LISTENING;
  s_speech_started = false;
  s_listen_start_ms = millis();
  s_rec_start_ms = millis();
  s_last_voice_ms = millis();
  s_voice_frames = 0;
  s_rec_max_vol = 0;
  s_ws.sendTXT("{\"type\":\"audio_start\"}");
  Serial.println(">>> 请说话...");
}

// ============================================================
// WebSocket 事件回调
// ============================================================
static void on_ws_event(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      s_ws_connected = true;
      Serial.println("[WS] 已连接 Mac 服务器");
      break;
    case WStype_DISCONNECTED:
      s_ws_connected = false;
      Serial.println("[WS] 与 Mac 服务器断开");
      break;
    case WStype_TEXT: {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, (const char *)payload, length);
      if (err) {
        Serial.printf("[WS] 收到非JSON: %.*s\n", (int)length, payload);
        break;
      }
      const char *t = doc["type"] | "";
      if (strcmp(t, "text") == 0) {
        const char *user  = doc["user"]  | "";
        const char *reply = doc["reply"] | "";
        Serial.printf(">>> 你说: %s\n", user);
        Serial.printf(">>> 小绿: %s\n", reply);
      } else if (strcmp(t, "tts_start") == 0) {        s_state = ST_PLAYING;        s_playing = true;
        s_tts_end_seen = false;
        Serial.println("[WS] TTS 开始");
      } else if (strcmp(t, "tts_end") == 0) {
        s_tts_end_seen = true;
        Serial.println("[WS] TTS 结束");
      } else if (strcmp(t, "error") == 0) {
        const char *msg = doc["message"] | "";
        Serial.printf("[WS] 错误: %s\n", msg);
      } else {
        Serial.printf("[WS] 收到: %.*s\n", (int)length, payload);
      }
      break;
    }
    case WStype_BIN:
      play_push(payload, length);
      break;
    default:
      break;
  }
}

static void init_websocket() {
  s_play_buf = (uint8_t *)ps_malloc(PLAY_BUFFER_SIZE);
  if (s_play_buf) {
    Serial.printf("[WS] 播放缓冲 %d KB 已分配(PSRAM)\n", PLAY_BUFFER_SIZE / 1024);
  } else {
    Serial.println("[WS] 警告：播放缓冲分配失败");
  }
  s_ws.begin(SERVER_HOST, SERVER_PORT, SERVER_PATH);
  s_ws.onEvent(on_ws_event);
  s_ws.setReconnectInterval(3000);
  Serial.printf("[WS] 目标服务器: ws://%s:%d%s\n", SERVER_HOST, SERVER_PORT, SERVER_PATH);
}

// ============================================================
// ESP-SR 初始化（加载 model 分区模型 + 创建识别器）
// ============================================================
static void init_speech_recognition() {
  srmodel_list_t *models = esp_srmodel_init("model");
  if (models == nullptr) {
    Serial.println("[SR] 错误：未找到 model 分区，请确认已烧录模型!");
    return;
  }
  Serial.println("[SR] model 分区加载成功");

  char *wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "nihaoxiaozhi");
  char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
  if (wn_name == nullptr || mn_name == nullptr) {
    Serial.printf("[SR] 错误：模型中未找到唤醒词(%s)或命令词(%s)\n",
                  wn_name ? wn_name : "NULL", mn_name ? mn_name : "NULL");
    return;
  }
  Serial.printf("[SR] 唤醒词模型: %s | 命令词模型: %s\n", wn_name, mn_name);

  Serial.println("[SR] 获取模型句柄...");
  s_wn = esp_wn_handle_from_name(wn_name);
  s_mn = esp_mn_handle_from_name(mn_name);
  if (s_wn == nullptr || s_mn == nullptr) {
    Serial.println("[SR] 错误：获取模型句柄失败");
    return;
  }
  Serial.println("[SR] 句柄获取成功");

  Serial.println("[SR] 创建唤醒词识别器...");
  s_wn_handle = s_wn->create(wn_name, DET_MODE_90);
  Serial.println("[SR] 唤醒词识别器创建成功");

  Serial.println("[SR] 创建命令词识别器...");
  s_mn_handle = s_mn->create(mn_name, MN_COMMAND_WINDOW_MS);
  Serial.println("[SR] 命令词识别器创建成功");

  if (s_wn_handle == nullptr || s_mn_handle == nullptr) {
    Serial.println("[SR] 错误：创建识别器失败");
    return;
  }

  Serial.printf("[SR] 唤醒词帧长=%d | 命令词帧长=%d | 采样率=%d\n",
                s_wn->get_samp_chunksize(s_wn_handle),
                s_mn->get_samp_chunksize(s_mn_handle),
                s_wn->get_samp_rate(s_wn_handle));
  Serial.println(">>> 语音识别就绪，请说唤醒词：你好小智");
}

// ============================================================
// 环形缓存
// ============================================================
static void pcm_push(const int16_t *src, int n) {
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

static bool pcm_take(int16_t *dst, int n) {
  if (s_pcm_len < n) return false;
  for (int i = 0; i < n; i++) dst[i] = s_pcm[(s_pcm_head + i) % PCM_BUFFER_SIZE];
  s_pcm_head = (s_pcm_head + n) % PCM_BUFFER_SIZE;
  s_pcm_len -= n;
  return true;
}

// ============================================================
// 命令处理（把识别到的命令映射成动作）
// ============================================================
static void handle_command(const char *cmd) {
  Serial.printf(">>> 识别到命令: [%s]\n", cmd);

  // TODO: 在这里接入你的设备动作，例如继电器/灯/风扇等
  // if (strcmp(cmd, "打开灯") == 0)     digitalWrite(GPIO_LED, HIGH);
  // else if (strcmp(cmd, "关闭灯") == 0) digitalWrite(GPIO_LED, LOW);
  // else if (strcmp(cmd, "打开风扇") == 0) ...
  // 注意：mn5_cn 内置命令词是固定的智能家居集合（开/关 空调、电视、灯、
  //       风扇、窗帘、加湿器、插座 等）。要自定义命令（如"浇花"）需换
  //       mn6_cn/mn7_cn 模型并配合 add_command()。
}

// ============================================================
// WiFi 连接（非阻塞，loop 里定时重连）
// ============================================================
static void wifi_connect() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WiFi] 连接中 %s ...\n", WIFI_SSID);
}

// ============================================================
// setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  wifi_connect();

  // 麦克风供电：GPIO13 输出 3.3V；L/R 接 GPIO17，拉低选左声道
  pinMode(MIC_VDD, OUTPUT);
  digitalWrite(MIC_VDD, HIGH);
  pinMode(MIC_LR, OUTPUT);
  digitalWrite(MIC_LR, LOW);
  delay(50);

  Serial.printf("[SYS] PSRAM: %s, %u bytes | 堆内存可用: %u | CPU: %u MHz\n",
                psramFound() ? "OK" : "FAIL",
                (unsigned)ESP.getPsramSize(),
                (unsigned)ESP.getFreeHeap(),
                (unsigned)getCpuFrequencyMhz());
  init_i2s();
  init_i2s_speaker();
  init_websocket();
  init_speech_recognition();
}

void loop() {
  // WiFi 状态显示 + 断线自动重连（5 秒一次，不阻塞主循环）
  if (WiFi.status() == WL_CONNECTED) {
    if (!s_wifi_online) {
      s_wifi_online = true;
      Serial.printf("[WiFi] 已连接! IP: %s\n", WiFi.localIP().toString().c_str());
    }
  } else {
    s_wifi_online = false;
    if (millis() - s_wifi_retry_ms > 5000) {
      s_wifi_retry_ms = millis();
      wifi_connect();
    }
  }

  s_ws.loop();   // WebSocket 收发（必须频繁调用）

  int wn_chunk = s_wn ? s_wn->get_samp_chunksize(s_wn_handle) : 0;
  if (wn_chunk <= 0) {
    delay(100);
    return;
  }

  // 读一帧 I2S（双声道交错 L/R，32bit -> 16bit PCM）
  static int32_t raw[1024];   // 512 帧 × 2 声道（static 避免占 loopTask 栈）
  size_t bytes_read = 0;
  if (i2s_read(I2S_PORT, raw, sizeof(raw), &bytes_read, portMAX_DELAY) != ESP_OK)
    return;
  int frames = bytes_read / 8;   // 每帧 = L(4B) + R(4B)
  static int16_t pcm[512];
  int16_t vol_l = 0;
  for (int i = 0; i < frames; i++) {
    int16_t l = (int16_t)(raw[2 * i] >> 14);      // 左声道（已确认数据在 L）
    pcm[i] = l;
    int16_t al = l < 0 ? -l : l;
    if (al > vol_l) vol_l = al;
  }
  pcm_push(pcm, frames);

  // ---- 环境噪声估计（空闲/聆听未说话时持续更新，播放时冻结） ----
  if (s_state == ST_IDLE || (s_state == ST_LISTENING && !s_speech_started)) {
    vad_update(vol_l);
  }

  // ---- 播放 TTS 音频 ----
  play_drain();

  // ---- 状态机：聆听 + 录音上传 ----
  if (s_state == ST_LISTENING) {
    if (!s_ws_connected) {
      // 服务器断开，放弃本轮并回待唤醒
      s_state = ST_IDLE;
      Serial.println(">>> 录音中断（服务器断开）");
    } else {
      s_ws.sendBIN((uint8_t *)pcm, frames * 2);
      uint32_t now = millis();

      if (!s_speech_started) {
        // 还没开始说话：等语音；先静置 GUARD_MS 避免扬声器余音误触发
        if (now - s_listen_start_ms >= GUARD_MS) {
          if (vol_l > s_voice_threshold) {
            s_speech_started = true;
            s_last_voice_ms = now;
            s_voice_frames = 1;
            if (vol_l > s_rec_max_vol) s_rec_max_vol = vol_l;
          } else if (now - s_listen_start_ms > CONV_TIMEOUT_MS) {
            // 连续对话超时，回到待唤醒
            s_state = ST_IDLE;
            Serial.println(">>> 对话结束（超时），回到待唤醒");
          }
        }
      } else {
        // 已开始说话：静音端点检测
        if (vol_l > s_rec_max_vol) s_rec_max_vol = vol_l;
        if (vol_l > s_voice_threshold) {
          s_last_voice_ms = now;
          s_voice_frames++;
        }
        if ((now - s_rec_start_ms > MIN_REC_MS && now - s_last_voice_ms > SILENCE_MS) ||
            now - s_rec_start_ms > MAX_REC_MS) {
          s_state = ST_PROCESSING;
          s_ws.sendTXT("{\"type\":\"audio_end\"}");
          Serial.printf(">>> 录音结束：时长=%lums 语音帧=%lu 峰值音量=%d 阈值=%u\n",
                        (unsigned long)(now - s_rec_start_ms),
                        (unsigned long)s_voice_frames,
                        (int)s_rec_max_vol,
                        (unsigned)s_voice_threshold);
        }
      }
    }
  }

  // ---- 唤醒词检测（仅在待唤醒状态运行） ----
  static int16_t frame[512];
  while (pcm_take(frame, wn_chunk)) {
    wakenet_state_t wn_state = s_wn->detect(s_wn_handle, frame);
    if (wn_state == WAKENET_DETECTED && s_state == ST_IDLE) {
      Serial.println(">>> 唤醒词命中：你好小智！");
      enter_listening();
    }
  }
}