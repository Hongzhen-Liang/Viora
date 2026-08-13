#include <Arduino.h>
#include <WiFi.h>
#include <driver/i2s.h>

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

// 音量指示（调试用）
static uint32_t s_vol_last_ms = 0;

// WiFi 重连计时
static uint32_t s_wifi_retry_ms = 0;
// WiFi 已连接标记（用于在连上时打印一次 IP）
static bool s_wifi_online = false;

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

  int wn_chunk = s_wn ? s_wn->get_samp_chunksize(s_wn_handle) : 0;
  int mn_chunk = s_mn ? s_mn->get_samp_chunksize(s_mn_handle) : 0;
  if (wn_chunk <= 0 || mn_chunk <= 0) {
    delay(100);
    return;
  }

  // 读一帧 I2S（双声道交错 L/R，32bit -> 16bit PCM）
  int32_t raw[1024];   // 512 帧 × 2 声道
  size_t bytes_read = 0;
  if (i2s_read(I2S_PORT, raw, sizeof(raw), &bytes_read, portMAX_DELAY) != ESP_OK)
    return;
  int frames = bytes_read / 8;   // 每帧 = L(4B) + R(4B)
  int16_t pcm[512];
  int16_t vol_l = 0, vol_r = 0;
  for (int i = 0; i < frames; i++) {
    int16_t l = (int16_t)(raw[2 * i] >> 14);      // 左声道
    int16_t r = (int16_t)(raw[2 * i + 1] >> 14);  // 右声道
    pcm[i] = l;                                    // 暂先喂左声道给识别
    int16_t al = l < 0 ? -l : l;
    int16_t ar = r < 0 ? -r : r;
    if (al > vol_l) vol_l = al;
    if (ar > vol_r) vol_r = ar;
  }
  pcm_push(pcm, frames);

  // ---- 双声道音量指示（诊断：看麦克风数据在 L 还是 R） ----
  if (millis() - s_vol_last_ms >= 1000) {
    Serial.printf("[VOL] L=%d R=%d（安静<500，说话>3000）\n", vol_l, vol_r);
    s_vol_last_ms = millis();
  }

  // ---- 唤醒词检测（连续喂帧） ----
  int16_t frame[512];
  while (pcm_take(frame, wn_chunk)) {
    wakenet_state_t wn_state = s_wn->detect(s_wn_handle, frame);
    if (wn_state == WAKENET_DETECTED) {
      Serial.println(">>> 唤醒词命中：你好小智！");
      s_woken = true;
    }
  }

  // ---- 唤醒后：命令词识别 ----
  if (s_woken) {
    while (pcm_take(frame, mn_chunk)) {
      esp_mn_state_t mn_state = s_mn->detect(s_mn_handle, frame);
      if (mn_state == ESP_MN_STATE_DETECTED) {
        esp_mn_results_t *res = s_mn->get_results(s_mn_handle);
        if (res != nullptr && res->num > 0) {
          handle_command(res->string);
        }
        s_woken = false;
        s_mn->clean(s_mn_handle);
      } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
        Serial.println(">>> 命令识别超时，回到待唤醒状态");
        s_woken = false;
        s_mn->clean(s_mn_handle);
      }
    }
  }
}