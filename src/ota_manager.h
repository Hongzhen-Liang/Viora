#pragma once

#include <Arduino.h>

// 在所有关键硬件/语音模块初始化后调用。core_health_ok=false
// 且当前是待验证 OTA 固件时，会自动回滚。
void ota_init(bool core_health_ok);

// 只在对话空闲时传 idle=true；下载和重启不会打断对话。
void ota_loop(bool idle);
void ota_request_check();

bool ota_enabled();
bool ota_busy();
const char *ota_status();
const char *ota_last_error();
const char *ota_running_slot();
