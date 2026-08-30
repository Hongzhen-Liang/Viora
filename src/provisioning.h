#pragma once
// ============================================================
// WiFi 配网模块：连不上网时开启 SoftAP + 网页，手机直接配置
// （iPhone 连上热点后，用浏览器访问 192.168.4.1 即可）。
// 联网后网页常驻于设备局域网 IP，可随时增删已保存的 WiFi。
// 配网期间 AP+STA 共存：保存新网络后不重启，STA 后台持续尝试连接。
// ============================================================
#include <Arduino.h>
#include <vector>

struct WifiCred {
  char ssid[33];   // SSID 最长 32 字符 + 结尾 '\0'
  char pass[65];   // WPA2 密码最长 63 字符 + 结尾 '\0'
};

// 启动时从 NVS 加载已保存网络，并拼出候选列表（保存的网络优先，编译期默认兜底）
void prov_setup();

// 候选网络列表（net 模块按顺序轮询尝试）
const std::vector<WifiCred> &prov_candidates();

bool prov_active();
void prov_begin(bool manual = false);  // 进入配网模式（AP + 网页 + DNS 劫持）
bool prov_should_close_on_connect();   // 手动配网需等目标网络连上再关闭热点
void prov_end();          // 退出配网模式（网页保留，联网后经设备 IP 访问）
void prov_loop();         // 网页服务循环，需在 main loop 频繁调用
void prov_web_refresh();  // WiFi 上线后重建网页监听（联网状态下管理页常驻）
