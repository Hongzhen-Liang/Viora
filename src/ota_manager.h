#pragma once

#include <Arduino.h>

// 在所有关键硬件/语音模块初始化后调用。core_health_ok=false
// 且当前是待验证 OTA 固件时，会自动回滚。
void ota_init(bool core_health_ok);

// 只在对话空闲时传 idle=true；检查、下载和安装均由用户分步确认，
// 且只会在空闲状态下执行。
void ota_loop(bool idle);
void ota_request_check();
void ota_request_download();
void ota_request_install();

enum class OtaUiEvent : uint8_t {
  kAvailable,
  kDownloading,
  kDownloadProgress,
  kReadyToInstall,
  kInstalling,
  kUpToDate,
  kError,
};

using OtaUiCallback = void (*)(OtaUiEvent event, const char *version,
                               uint32_t value);
void ota_set_ui_callback(OtaUiCallback callback);

bool ota_enabled();
bool ota_busy();
bool ota_update_available();
bool ota_ready_to_install();
const char *ota_available_version();
uint32_t ota_available_build();
const char *ota_status();
const char *ota_last_error();
const char *ota_running_slot();
