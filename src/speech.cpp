// ============================================================
// 语音模块实现：esp-sr wakenet
// ============================================================
#include <Arduino.h>

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include "config.h"
#include "speech.h"

static const esp_wn_iface_t *s_wn = nullptr;
static model_iface_data_t   *s_wn_handle = nullptr;

bool speech_init() {
  srmodel_list_t *models = esp_srmodel_init("model");
  if (models == nullptr) {
    Serial.println("[SR] 错误：未找到 model 分区，请确认已烧录模型!");
    return false;
  }
  Serial.println("[SR] model 分区加载成功");

  char *wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "nihaoxiaozhi");
  if (wn_name == nullptr) {
    Serial.println("[SR] 错误：模型中未找到唤醒词");
    return false;
  }
  Serial.printf("[SR] 唤醒词模型: %s\n", wn_name);

  Serial.println("[SR] 获取模型句柄...");
  s_wn = esp_wn_handle_from_name(wn_name);
  if (s_wn == nullptr) {
    Serial.println("[SR] 错误：获取唤醒词模型句柄失败");
    return false;
  }
  Serial.println("[SR] 句柄获取成功");

  Serial.println("[SR] 创建唤醒词识别器...");
  s_wn_handle = s_wn->create(wn_name, DET_MODE_90);
  if (s_wn_handle == nullptr) {
    Serial.println("[SR] 错误：创建唤醒词识别器失败");
    return false;
  }
  Serial.println("[SR] 唤醒词识别器创建成功");

  Serial.printf("[SR] 唤醒词帧长=%d | 采样率=%d\n",
                s_wn->get_samp_chunksize(s_wn_handle),
                s_wn->get_samp_rate(s_wn_handle));
  Serial.println(">>> 语音识别就绪，请说唤醒词：你好小智");
  return true;
}

int speech_chunk_size() {
  return (s_wn && s_wn_handle) ? s_wn->get_samp_chunksize(s_wn_handle) : 0;
}

bool speech_detect(const int16_t *frame) {
  if (!s_wn || !s_wn_handle) return false;
  return s_wn->detect(s_wn_handle, (int16_t *)frame) == WAKENET_DETECTED;
}
