// ============================================================
// WiFi 配网模块实现
// 连不上 WiFi 时：ESP32 开启热点 Viora-Setup + 网页（DNS 劫持），
// 手机连上热点用浏览器即可修改 WiFi，无需电脑。
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <vector>

#include "config.h"
#include "provisioning.h"

static WebServer s_web(80);
static DNSServer s_dns;
static Preferences s_prefs;

static std::vector<WifiCred> s_saved;      // NVS 里保存的网络（新存的在前）
static std::vector<WifiCred> s_candidates; // 候选列表 = 已保存 + 编译期默认
static bool s_active = false;
static bool s_restart_pending = false;
static uint32_t s_restart_ms = 0;

// ---------- NVS 持久化 ----------
static void save_all_to_nvs() {
  if (!s_prefs.begin("viora", false)) {
    Serial.println("[Prov] NVS 打开失败（写入）");
    return;
  }
  s_prefs.putUChar("net_count", static_cast<uint8_t>(s_saved.size()));
  for (size_t i = 0; i < s_saved.size(); i++) {
    char k1[16], k2[16];
    snprintf(k1, sizeof(k1), "ssid%u", static_cast<unsigned>(i));
    snprintf(k2, sizeof(k2), "pass%u", static_cast<unsigned>(i));
    s_prefs.putString(k1, s_saved[i].ssid);
    s_prefs.putString(k2, s_saved[i].pass);
  }
  s_prefs.end();
}

static void rebuild_candidates() {
  s_candidates = s_saved;
  // secrets.h 里的默认网络兜底（去重）
  if (WIFI_SSID[0] != '\0') {
    bool has = false;
    for (const auto &c : s_candidates) {
      if (strcmp(c.ssid, WIFI_SSID) == 0) {
        has = true;
        break;
      }
    }
    if (!has) {
      WifiCred c{};
      strlcpy(c.ssid, WIFI_SSID, sizeof(c.ssid));
      strlcpy(c.pass, WIFI_PASS, sizeof(c.pass));
      s_candidates.push_back(c);
    }
  }
}

void prov_setup() {
  s_saved.clear();
  if (s_prefs.begin("viora", true)) {
    uint8_t n = s_prefs.getUChar("net_count", 0);
    if (n > PROV_MAX_NETWORKS) n = PROV_MAX_NETWORKS;
    for (uint8_t i = 0; i < n; i++) {
      char k1[16], k2[16];
      snprintf(k1, sizeof(k1), "ssid%u", static_cast<unsigned>(i));
      snprintf(k2, sizeof(k2), "pass%u", static_cast<unsigned>(i));
      WifiCred c{};
      s_prefs.getString(k1, c.ssid, sizeof(c.ssid));
      s_prefs.getString(k2, c.pass, sizeof(c.pass));
      if (c.ssid[0] != '\0') s_saved.push_back(c);
    }
    s_prefs.end();
  }
  rebuild_candidates();
  Serial.printf("[Prov] 已保存 WiFi: %u 个，候选共 %u 个\n",
                static_cast<unsigned>(s_saved.size()),
                static_cast<unsigned>(s_candidates.size()));
}

const std::vector<WifiCred> &prov_candidates() { return s_candidates; }

// ---------- 增删网络 ----------
static void save_cred(const char *ssid, const char *pass) {
  // 已有同 SSID：更新密码并挪到最前（优先尝试）
  for (auto it = s_saved.begin(); it != s_saved.end(); ++it) {
    if (strcmp(it->ssid, ssid) == 0) {
      WifiCred c = *it;
      strlcpy(c.pass, pass, sizeof(c.pass));
      s_saved.erase(it);
      s_saved.insert(s_saved.begin(), c);
      save_all_to_nvs();
      rebuild_candidates();
      return;
    }
  }
  // 新网络插到最前，超出上限淘汰最旧的
  WifiCred c{};
  strlcpy(c.ssid, ssid, sizeof(c.ssid));
  strlcpy(c.pass, pass, sizeof(c.pass));
  s_saved.insert(s_saved.begin(), c);
  while (s_saved.size() > PROV_MAX_NETWORKS) s_saved.pop_back();
  save_all_to_nvs();
  rebuild_candidates();
}

static void remove_cred(size_t idx) {
  if (idx >= s_saved.size()) return;
  s_saved.erase(s_saved.begin() + static_cast<ptrdiff_t>(idx));
  save_all_to_nvs();
  rebuild_candidates();
}

// ---------- 网页 ----------
static String html_escape(const char *s) {
  String out;
  out.reserve(strlen(s) + 16);
  for (const char *p = s; *p; p++) {
    switch (*p) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += *p; break;
    }
  }
  return out;
}

static String page_head(const char *title) {
  String h;
  h.reserve(768);
  h += F("<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">");
  h += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  h += F("<title>");
  h += title;
  h += F("</title><style>");
  h += F("body{font-family:-apple-system,BlinkMacSystemFont,'PingFang SC',sans-serif;background:#0f1420;color:#e8ecf4;margin:0;padding:24px 20px}");
  h += F(".card{background:#1a2130;border-radius:14px;padding:18px;margin-bottom:16px}");
  h += F("h1{font-size:22px;margin:0 0 6px}h2{font-size:16px;margin:0 0 10px;color:#9fb3d1}");
  h += F("label{display:block;font-size:13px;color:#9fb3d1;margin:10px 0 4px}");
  h += F("input{width:100%;box-sizing:border-box;background:#0f1420;border:1px solid #2c3a55;color:#e8ecf4;border-radius:10px;padding:12px;font-size:16px}");
  h += F("button{width:100%;margin-top:16px;background:#3b82f6;border:0;color:#fff;border-radius:10px;padding:13px;font-size:16px;font-weight:600}");
  h += F("ul{list-style:none;margin:0;padding:0}li{display:flex;justify-content:space-between;align-items:center;padding:9px 0;border-bottom:1px solid #22304a;font-size:15px}");
  h += F("a{color:#f87171;text-decoration:none;font-size:13px}");
  h += F(".tip{font-size:12px;color:#7d8da8;line-height:1.6}");
  h += F("</style></head><body>");
  return h;
}

static String portal_page() {
  String h = page_head("Viora WiFi 配网");
  h += F("<h1>Viora WiFi 配网</h1>");
  h += F("<p class=\"tip\">设备连不上网络，已进入配网模式。填写新 WiFi，保存后设备会自动重启并连接。</p>");
  h += F("<form class=\"card\" method=\"post\" action=\"/save\">");
  h += F("<label>WiFi 名称（SSID）</label><input name=\"ssid\" required autofocus autocapitalize=\"off\" autocorrect=\"off\">");
  h += F("<label>WiFi 密码</label><input name=\"pass\" type=\"password\" placeholder=\"开放网络可留空\">");
  h += F("<button type=\"submit\">保存并重启</button></form>");
  h += F("<div class=\"card\"><h2>已保存的网络（按顺序尝试）</h2><ul>");
  if (s_saved.empty()) {
    h += F("<li><span>暂无（只有默认网络）</span></li>");
  }
  for (size_t i = 0; i < s_saved.size(); i++) {
    h += F("<li><span>");
    h += html_escape(s_saved[i].ssid);
    h += F("</span><a href=\"/del?i=");
    h += String(static_cast<unsigned>(i));
    h += F("\">删除</a></li>");
  }
  h += F("</ul></div>");
  h += F("<p class=\"tip\">若此页面未自动弹出，请在浏览器地址栏输入 192.168.4.1。</p>");
  h += F("</body></html>");
  return h;
}

static String saved_page(const char *ssid) {
  String h = page_head("已保存");
  h += F("<h1>已保存</h1><p>WiFi <b>");
  h += html_escape(ssid);
  h += F("</b> 已保存，设备正在重启…</p>");
  h += F("<p class=\"tip\">重启后设备会自动连接该网络。连接成功后热点自动关闭、状态灯变蓝色呼吸；若连不上，90 秒后会再次进入配网模式。</p>");
  h += F("</body></html>");
  return h;
}

static String message_page(const char *title, const char *msg) {
  String h = page_head(title);
  h += F("<h1>");
  h += title;
  h += F("</h1><p>");
  h += msg;
  h += F("</p><p class=\"tip\"><a href=\"/\">返回配网页</a></p></body></html>");
  return h;
}

// ---------- 请求处理 ----------
static void handle_portal() {
  s_web.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  s_web.send(200, "text/html", portal_page());
}

static void handle_save() {
  String ssid = s_web.arg("ssid");
  String pass = s_web.arg("pass");
  ssid.trim();
  if (ssid.length() < 1 || ssid.length() >= 33) {
    s_web.send(200, "text/html",
               message_page("SSID 无效", "WiFi 名称长度需在 1~32 个字符。"));
    return;
  }
  if (pass.length() >= 64) {
    s_web.send(200, "text/html",
               message_page("密码过长", "WiFi 密码最长 63 个字符。"));
    return;
  }
  save_cred(ssid.c_str(), pass.c_str());
  Serial.printf("[Prov] 保存 WiFi: %s（密码 %d 位），准备重启\n", ssid.c_str(),
                static_cast<int>(pass.length()));
  s_web.send(200, "text/html", saved_page(ssid.c_str()));
  s_restart_pending = true;
  s_restart_ms = millis();
}

static void handle_del() {
  const int i = atoi(s_web.arg("i").c_str());
  if (i >= 0) {
    remove_cred(static_cast<size_t>(i));
    Serial.printf("[Prov] 删除已保存 WiFi #%d\n", i);
  }
  s_web.sendHeader("Location", "/", true);
  s_web.send(302, "text/plain", "");
}

// ---------- 对外接口 ----------
bool prov_active() { return s_active; }

void prov_begin() {
  if (s_active) return;
  Serial.printf("[Prov] WiFi 连续 %lu 毫秒未连上，进入配网模式\n",
                static_cast<unsigned long>(PROV_TIMEOUT_MS));
  Serial.printf("[Prov] 热点: %s（密码 %s），请用手机连接后访问 http://192.168.4.1\n",
                PROV_AP_SSID, PROV_AP_PASS[0] ? PROV_AP_PASS : "无");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  WiFi.softAP(PROV_AP_SSID, PROV_AP_PASS);
  delay(100);
  s_dns.setErrorReplyCode(DNSReplyCode::NoError);
  s_dns.start(53, "*", WiFi.softAPIP());
  s_web.on("/save", HTTP_POST, handle_save);
  s_web.on("/del", HTTP_GET, handle_del);
  s_web.onNotFound(handle_portal);
  s_web.begin();
  s_active = true;
}

void prov_end() {
  if (!s_active) return;
  s_web.stop();
  s_dns.stop();
  WiFi.softAPdisconnect(true);
  s_active = false;
  Serial.println("[Prov] 已退出配网模式");
}

void prov_loop() {
  s_dns.processNextRequest();
  s_web.handleClient();
  if (s_restart_pending && millis() - s_restart_ms > 1500) {
    Serial.println("[Prov] 重启设备以连接新 WiFi …");
    ESP.restart();
  }
}
