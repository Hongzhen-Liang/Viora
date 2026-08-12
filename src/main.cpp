#include <Arduino.h>
#include <driver/i2s.h>

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "model_path.h"

// ============================================================
// 引脚定义（MSM3526 / INMP441，I2S 数字 MEMS 麦克风）
// 选择理由：三线相邻（左排针 GPIO15/16/17）、避开 USB(19/20)、
// 八线PSRAM(26-37)、strapping(0/3/45/46)，且非 ADC，把 ADC1 留给植物传感器
// 模块物理排布：一排是 [SD][VDD][GND]，另一排是 [SCK][WS][L/R]
// MIC_VDD：GPIO13 软件输出 3.3V 给麦克风供电（电流仅 ~1.4mA，安全）
// ============================================================
#define I2S_SCK  15
#define I2S_WS   16
#define I2S_SD   17
#define MIC_VDD  13   // 输出高电平 ≈3.3V，给麦克风 VDD 供电
#define I2S_PORT I2S_NUM_0

// 采样率（ESP-SR 固定要求 16kHz）
#define SR_SAMPLE_RATE 16000

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

// ============================================================
// I2S 初始化
// ============================================================
static void init_i2s() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SR_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // INMP441 是 24bit，装在 32bit 帧里
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
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
// setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  // 用 GPIO14 输出 3.3V 给麦克风 VDD 供电（需先上电再初始化 I2S）
  pinMode(MIC_VDD, OUTPUT);
  digitalWrite(MIC_VDD, HIGH);
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
  int wn_chunk = s_wn ? s_wn->get_samp_chunksize(s_wn_handle) : 0;
  int mn_chunk = s_mn ? s_mn->get_samp_chunksize(s_mn_handle) : 0;
  if (wn_chunk <= 0 || mn_chunk <= 0) {
    delay(100);
    return;
  }

  // 读一帧 I2S（32bit -> 16bit PCM）
  int32_t raw[512];
  size_t bytes_read = 0;
  if (i2s_read(I2S_PORT, raw, sizeof(raw), &bytes_read, portMAX_DELAY) != ESP_OK)
    return;
  int n = bytes_read / 4;
  int16_t pcm[512];
  for (int i = 0; i < n; i++) pcm[i] = (int16_t)(raw[i] >> 14);  // 24bit->16bit
  pcm_push(pcm, n);

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