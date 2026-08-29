#pragma once
// ============================================================
// 敏感配置模板（会提交到 GitHub）
// 复制本文件为 src/secrets.h 后填写真实值；secrets.h 已被 git 忽略。
// 缺失 secrets.h 时，config.h 回退到本地开发默认值（连不上真实服务器）。
// ============================================================

// WiFi
#define SECRET_WIFI_SSID "your-wifi-ssid"
#define SECRET_WIFI_PASS "your-wifi-password"

// HTTPS/WSS 服务器；域名必须能解析，并且要与服务器证书匹配。
// 若 ESP32 在局域网内使用，建议用局域网 DNS 将该域名解析到 Mac；不要随意改成 IP。
#define SECRET_SERVER_HOST "your-server-host"
#define SECRET_SERVER_PORT 11451

// 与 VioraServer/.env 里的 API_KEY 保持一致；服务端留空则鉴权关闭。
#define SECRET_API_KEY "your-api-key"

// 扫描设备屏幕二维码后打开的绑定页。请填你自己可访问的 HTTPS 网站，
// 例如 https://viora.example.cn/bind；不要再使用 chatgpt.site。
// 查看码位于 URL 的 # 片段中，不会随 HTTP 请求发送到服务器。
#define SECRET_BINDING_WEB_URL "https://your-viora.example.cn/bind"

// ESP32 到语音服务器强制使用 wss://。默认复用 ota_secrets.h 中的
// Let's Encrypt 根证书；若服务器使用其他 CA，再定义 SECRET_SERVER_ROOT_CA。
#define SECRET_SERVER_TLS_ENABLED 1

// OTA：必须是 HTTPS。OTA token 建议与语音服务 API Key 分开。
#define SECRET_OTA_MANIFEST_URL "https://your-server.example.com/api/firmware/manifest"
#define SECRET_OTA_API_KEY "your-per-device-ota-token"

// 粘贴签发 OTA 服务器 HTTPS 证书的根 CA PEM。不允许留空或跳过校验。
static const char SECRET_OTA_ROOT_CA_VALUE[] = R"PEM(
-----BEGIN CERTIFICATE-----
replace-with-your-root-ca
-----END CERTIFICATE-----
)PEM";
#define SECRET_OTA_ROOT_CA SECRET_OTA_ROOT_CA_VALUE
