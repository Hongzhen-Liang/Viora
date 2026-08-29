#pragma once

#include <Arduino.h>

// 端到端遥测：密钥只保存在 ESP32 的 NVS 与用户浏览器本地。
// Server/Web 只接触 AES-GCM 密文与公开元数据。
bool secure_telemetry_init();
const char *secure_telemetry_device_id();
const char *secure_telemetry_pairing_code();
const char *secure_telemetry_key_fingerprint();
const char *secure_telemetry_viewing_key();
uint32_t secure_telemetry_next_sequence();

// 把 JSON 传感器对象封装为 telemetry_encrypted WebSocket 帧。
// out 必须至少容纳 1024 字节。
bool secure_telemetry_encrypt(const char *plaintext_json,
                               uint32_t sequence,
                               uint64_t captured_at,
                               char *out,
                               size_t out_size);

// 连接 WebSocket 后发送一次设备登记信息。登记信息不包含设备密钥，
// 只包含用于配对校验的指纹。
bool secure_telemetry_build_pairing(char *out, size_t out_size);

// 生成给设备屏幕二维码使用的绑定页 URL。查看码只放在 URL fragment
// （# 后），不会被浏览器作为 HTTP 请求发送给 Web 服务器。
bool secure_telemetry_build_binding_url(char *out, size_t out_size);
