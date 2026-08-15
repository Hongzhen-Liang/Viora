#pragma once
// ============================================================
// 敏感配置模板（会提交到 GitHub）
// 复制本文件为 src/secrets.h 后填写真实值；secrets.h 已被 git 忽略。
// 缺失 secrets.h 时，config.h 回退到本地开发默认值（连不上真实服务器）。
// ============================================================

// WiFi
#define SECRET_WIFI_SSID "your-wifi-ssid"
#define SECRET_WIFI_PASS "your-wifi-password"

// Mac 服务器（WebSocket）
// 域名需要局域网 DNS 能解析到 Mac；也可以直接填局域网 IP。
#define SECRET_SERVER_HOST "your-server-host"
#define SECRET_SERVER_PORT 8765

// 与 VioraServer/.env 里的 API_KEY 保持一致；服务端留空则鉴权关闭。
#define SECRET_API_KEY "your-api-key"
