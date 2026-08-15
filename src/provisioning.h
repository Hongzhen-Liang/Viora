#pragma once
// ============================================================
// WiFi 配网模块：连不上网时开启 SoftAP + 网页，手机直接配置
// （iPhone 连上热点后，用浏览器访问 192.168.4.1 即可）
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
void prov_begin();   // 进入配网模式（AP + 网页 + DNS 劫持）
void prov_end();     // 退出配网模式
void prov_loop();    // 网页服务循环，需在 main loop 频繁调用
