// ============================================================
// WiFi 配网模块实现
// 连不上 WiFi 时：ESP32 开启热点 Viora-Setup + 网页（DNS 劫持），
// 手机连上热点用浏览器即可修改 WiFi，无需电脑。
// 联网后：同一网页常驻于设备局域网 IP（http://设备IP/），
// 可随时增删已保存的 WiFi。
// 配网期间 AP+STA 共存：保存新网络后不重启，STA 后台持续尝试连接，
// 解决 iPhone 个人热点场景的"重启 vs 热点广播"时机竞争。
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <vector>

#include "config.h"
#include "net.h"
#include "ota_manager.h"
#include "provisioning.h"

static WebServer s_web(80);
static DNSServer s_dns;
static Preferences s_prefs;

static std::vector<WifiCred> s_saved;      // NVS 里保存的网络（新存的在前）
static std::vector<WifiCred> s_candidates; // 候选列表 = 已保存 + 编译期默认
static bool s_active = false;
static bool s_manual_mode = false;
static bool s_web_running = false;         // 网页服务是否已在监听（配网与联网共用）
static char s_waiting_ssid[33];            // 保存后正在后台等待连接的目标 SSID

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
  h += F(".steps{list-style:decimal;padding-left:20px;margin:6px 0 0}");
  h += F(".steps li{display:list-item;border-bottom:0;padding:4px 0;font-size:14px;line-height:1.7;color:#e8ecf4;text-align:left}");
  h += F("</style></head><body>");
  return h;
}

static String portal_page() {
  String h = page_head("Viora WiFi 管理");
  h += F("<h1>Viora WiFi 管理</h1>");
  if (s_active) {
    h += F("<p class=\"tip\">设备连不上网络，已进入配网模式。填写新 WiFi 保存后，设备会立即在后台尝试连接（不重启），本页面保持在线。</p>");
  } else if (WiFi.status() == WL_CONNECTED) {
    h += F("<p class=\"tip\">设备已联网。本页即局域网管理页，浏览器访问 <b>http://");
    h += WiFi.localIP().toString();
    h += F("/</b> 随时可打开；在下方增删已保存的 WiFi。页面受密码保护，浏览器会提示登录。</p>");
  }
  if (s_web.arg("saved") == "1") {
    h += F("<div class=\"card\"><h2>已保存新网络</h2><p class=\"tip\">设备正在尝试连接。若目标网络不可用，会自动按顺序回连其他已保存网络。</p></div>");
  }
  h += F("<form class=\"card\" method=\"post\" action=\"/save\">");
  h += F("<label>WiFi 名称（SSID）</label><input name=\"ssid\" required autofocus autocapitalize=\"off\" autocorrect=\"off\">");
  h += F("<label>WiFi 密码</label><input name=\"pass\" type=\"password\" placeholder=\"开放网络可留空\">");
  h += F("<button type=\"submit\">保存并连接</button></form>");
  if (s_active && s_waiting_ssid[0] != '\0') {
    h += F("<div class=\"card\"><h2>正在后台尝试连接</h2><p>目标网络：<b>");
    h += html_escape(s_waiting_ssid);
    h += F("</b></p><p class=\"tip\">白色脉冲灯 = 等待网络出现。可随时离开本页面；未成功时重新连回本热点即可修改。</p></div>");
  }
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
  h += F("<div class=\"card\"><h2>固件</h2><p>当前：<b>");
  h += FIRMWARE_VERSION;
  h += F("</b> (build ");
  h += String(FIRMWARE_BUILD);
  h += F(") · 槽 ");
  h += ota_running_slot();
  h += F("</p><p class=\"tip\">状态：");
  h += ota_status();
  if (ota_last_error()[0] != '\0') {
    h += F(" · ");
    h += html_escape(ota_last_error());
  }
  h += F("</p><a href=\"/ota\" style=\"color:#60a5fa\">打开固件更新</a></div>");
  if (s_active) {
    h += F("<p class=\"tip\">若此页面未自动弹出，请在浏览器地址栏输入 192.168.4.1。</p>");
  } else {
    h += F("<p class=\"tip\">保存新网络后设备会立即切换连接；访问管理页的浏览器连接会随切网短暂中断。</p>");
  }
  h += F("</body></html>");
  return h;
}

static String waiting_page(const char *ssid) {
  String h = page_head("已保存，正在连接");
  h += F("<h1>已保存，正在连接</h1><p>WiFi <b>");
  h += html_escape(ssid);
  h += F("</b> 已保存。设备已在后台开始尝试连接（不重启），本配网热点会保持在线。</p>");
  h += F("<div class=\"card\"><h2>iPhone 个人热点用户请按以下步骤</h2><ol class=\"steps\">");
  h += F("<li>确认\"设置 → 个人热点\"里的 <b>最大兼容性</b> 已开启（ESP32 只支持 2.4GHz）。</li>");
  h += F("<li>断开本 WiFi（Viora-Setup），回到个人热点页面并 <b>停留在该页</b> 直到设备连上。</li>");
  h += F("<li>连接成功后：状态灯蓝色呼吸，个人热点页会出现设备 \"Viora\"。</li>");
  h += F("<li>一段时间未连上（如密码错误）：重新连回 Viora-Setup，在本页面修改。</li>");
  h += F("</ol></div>");
  h += F("<p class=\"tip\">离开本页面后反馈看指示灯：白色脉冲 = 等待网络；蓝色呼吸 = 已连接。</p>");
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
// ---------- HTTP Basic 鉴权 ----------
// 联网模式下管理页整页受密码保护（浏览器自动弹登录框，用户名固定
// admin，密码 = config.h 的 WEB_ADMIN_PASS）。浏览器记住凭据后，
// 对同一主机的后续请求都会自动携带，表单 POST 与删除链接无需再传密码。
// 配网热点模式下不二次鉴权（热点密码即门槛），见 page_access_ok()。
static void send_unauthorized() {
  s_web.sendHeader("WWW-Authenticate", "Basic realm=\"Viora\"", true);
  s_web.send(401, "text/plain", "请输入管理密码");
}

// 极简 RFC4648 Base64 编码：把 "admin:密码" 编码后与浏览器发来的
// Authorization 头比对，避免引入额外依赖。
static String b64_encode(const char *s) {
  static const char kTab[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out;
  const size_t len = strlen(s);
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t v =
        (static_cast<uint32_t>(static_cast<unsigned char>(s[i])) << 16) |
        (i + 1 < len ? static_cast<uint32_t>(static_cast<unsigned char>(s[i + 1])) << 8 : 0) |
        (i + 2 < len ? static_cast<uint32_t>(static_cast<unsigned char>(s[i + 2])) : 0);
    out += kTab[(v >> 18) & 63];
    out += kTab[(v >> 12) & 63];
    out += (i + 1 < len) ? kTab[(v >> 6) & 63] : '=';
    out += (i + 2 < len) ? kTab[v & 63] : '=';
  }
  return out;
}

static bool basic_auth_ok() {
  if (WEB_ADMIN_PASS[0] == '\0') return true;  // 未设置密码 = 免密
  if (!s_web.hasHeader("Authorization")) return false;
  const String auth = s_web.header("Authorization");
  if (!auth.startsWith("Basic ")) return false;
  char expected[96];
  snprintf(expected, sizeof(expected), "admin:%s", WEB_ADMIN_PASS);
  return auth.substring(6) == b64_encode(expected);
}

// 页面访问策略（iPhone 友好）：
// - 配网模式（SoftAP 热点）：连上 Viora-Setup 本身已需要热点密码，
//   不再二次鉴权——iOS 系统 captive portal 不支持 Basic 401 弹窗，
//   免密可保证 iPhone 连上热点后自动弹出配网页；
// - 联网模式（局域网）：整页 Basic 鉴权，防止局域网内任何设备改 WiFi。
static bool page_access_ok() {
  if (s_active) return true;
  return basic_auth_ok();
}

static void handle_portal() {
  if (!page_access_ok()) {
    send_unauthorized();
    return;
  }
  s_web.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  s_web.send(200, "text/html", portal_page());
}

static void handle_save() {
  if (!page_access_ok()) {
    send_unauthorized();
    return;
  }
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
  strlcpy(s_waiting_ssid, ssid.c_str(), sizeof(s_waiting_ssid));
  Serial.printf("[Prov] 保存 WiFi: %s（密码 %d 位），开始后台连接（AP+STA 共存）\n",
                ssid.c_str(), static_cast<int>(pass.length()));
  if (s_active) {
    s_web.send(200, "text/html", waiting_page(ssid.c_str()));
  } else {
    // 联网状态下切网会中断当前浏览器连接，直接回管理页并提示
    s_web.sendHeader("Location", "/?saved=1", true);
    s_web.send(302, "text/plain", "");
  }
  net_wifi_retry_now();  // 新网络在候选首位，立即开始尝试
}

static void handle_del() {
  if (!page_access_ok()) {
    send_unauthorized();
    return;
  }
  const int i = atoi(s_web.arg("i").c_str());
  if (i >= 0 && static_cast<size_t>(i) < s_saved.size()) {
    // 删掉正在等待连接的目标网络时，同步清掉等待状态
    const bool was_waiting =
        strcmp(s_saved[static_cast<size_t>(i)].ssid, s_waiting_ssid) == 0;
    remove_cred(static_cast<size_t>(i));
    if (was_waiting) s_waiting_ssid[0] = '\0';
    Serial.printf("[Prov] 删除已保存 WiFi #%d\n", i);
  }
  s_web.sendHeader("Location", "/", true);
  s_web.send(302, "text/plain", "");
}

static String ota_page() {
  String h = page_head("Viora 固件更新");
  h += F("<h1>固件更新</h1><div class=\"card\"><h2>设备状态</h2><p>版本：<b>");
  h += FIRMWARE_VERSION;
  h += F("</b> (build ");
  h += String(FIRMWARE_BUILD);
  h += F(")</p><p>运行槽：");
  h += ota_running_slot();
  h += F("</p><p>更新状态：");
  h += ota_status();
  h += F("</p>");
  if (ota_update_available()) {
    h += F("<p>可用版本：<b>");
    h += html_escape(ota_available_version());
    h += F("</b> (build ");
    h += String(ota_available_build());
    h += F(")</p>");
  }
  if (ota_last_error()[0] != '\0') {
    h += F("<p class=\"tip\">最近错误：");
    h += html_escape(ota_last_error());
    h += F("</p>");
  }
  h += F("</div>");
  if (ota_enabled()) {
    h += F("<form class=\"card\" method=\"post\" action=\"/ota/check\"><p class=\"tip\">只检查版本信息，不会自动下载或重启。</p><button type=\"submit\">检查新版本</button></form>");
    if (ota_ready_to_install()) {
      h += F("<form class=\"card\" method=\"post\" action=\"/ota/install\"><p class=\"tip\">固件已下载并通过摘要、签名校验。安装会重启设备，并在健康检查失败时回滚。</p><button type=\"submit\">安装并重启</button></form>");
    } else if (ota_update_available()) {
      h += F("<form class=\"card\" method=\"post\" action=\"/ota/download\"><p class=\"tip\">下载期间请保持设备通电；下载完成后仍需再次确认安装。</p><button type=\"submit\">下载更新</button></form>");
    }
  } else {
    h += F("<div class=\"card\"><p class=\"tip\">OTA 尚未配置。需在固件 ota_secrets.h 中设置 HTTPS manifest 地址、token 和根 CA。</p></div>");
  }
  h += F("<p class=\"tip\"><a href=\"/\" style=\"color:#60a5fa\">返回 WiFi 管理</a></p></body></html>");
  return h;
}

static void handle_ota() {
  if (!page_access_ok()) {
    send_unauthorized();
    return;
  }
  s_web.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  s_web.send(200, "text/html", ota_page());
}

static void handle_ota_check() {
  // 升级操作始终需要 Basic 凭据，即使此时设备恰好也开着
  // 配网热点；连上热点本身不应该获得固件控制权。
  if (!basic_auth_ok()) {
    send_unauthorized();
    return;
  }
  ota_request_check();
  s_web.sendHeader("Location", "/ota", true);
  s_web.send(303, "text/plain", "");
}

static void handle_ota_download() {
  if (!basic_auth_ok()) {
    send_unauthorized();
    return;
  }
  ota_request_download();
  s_web.sendHeader("Location", "/ota", true);
  s_web.send(303, "text/plain", "");
}

static void handle_ota_install() {
  if (!basic_auth_ok()) {
    send_unauthorized();
    return;
  }
  ota_request_install();
  s_web.sendHeader("Location", "/ota", true);
  s_web.send(303, "text/plain", "");
}

// ---------- 网页服务生命周期 ----------
// 配网与联网两种状态共用同一个 80 端口网页：
// 配网时经 192.168.4.1 访问；联网时经设备局域网 IP 访问，随时增删已保存 WiFi。
static void web_start() {
  if (s_web_running) return;
  s_web.on("/save", HTTP_POST, handle_save);
  s_web.on("/del", HTTP_GET, handle_del);
  s_web.on("/ota", HTTP_GET, handle_ota);
  s_web.on("/ota/check", HTTP_POST, handle_ota_check);
  s_web.on("/ota/download", HTTP_POST, handle_ota_download);
  s_web.on("/ota/install", HTTP_POST, handle_ota_install);
  s_web.onNotFound(handle_portal);
  s_web.begin();
  s_web_running = true;
}

static void web_restart() {
  s_web.stop();
  s_web_running = false;
  web_start();
}

// ---------- 对外接口 ----------
bool prov_active() { return s_active; }

void prov_begin(bool manual) {
  if (s_active) {
    if (manual) s_manual_mode = true;
    return;
  }
  s_manual_mode = manual;
  if (manual) Serial.println("[Prov] 用户手动进入配网模式");
  else Serial.printf("[Prov] WiFi 连续 %lu 毫秒未连上，进入配网模式\n",
                     static_cast<unsigned long>(PROV_TIMEOUT_MS));
  Serial.printf("[Prov] 热点: %s（密码 %s），请用手机连接后访问 http://192.168.4.1\n",
                PROV_AP_SSID, PROV_AP_PASS[0] ? PROV_AP_PASS : "无");
  // AP+STA 共存：配网页保持在线，STA 后台继续尝试连接。iPhone 热点场景
  // 下用户保存后离开本热点、再打开个人热点，设备持续等待即可，无需重启。
  // disconnect 不能带 eraseap 参数（true），否则会清掉 STA 配置。
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  WiFi.softAP(PROV_AP_SSID, PROV_AP_PASS);
  delay(100);
  s_dns.setErrorReplyCode(DNSReplyCode::NoError);
  s_dns.start(53, "*", WiFi.softAPIP());
  web_start();
  s_active = true;
}

bool prov_should_close_on_connect() {
  if (!s_manual_mode) return true;
  return s_waiting_ssid[0] != '\0' &&
         strcmp(WiFi.SSID().c_str(), s_waiting_ssid) == 0;
}

void prov_end() {
  if (!s_active) return;
  s_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);  // STA 已连上，切回纯 STA 省电
  s_active = false;
  s_manual_mode = false;
  s_waiting_ssid[0] = '\0';
  web_restart();  // 模式切换会重建网络接口，重启监听让管理页在局域网继续可用
  Serial.println("[Prov] 已退出配网模式");
}

// WiFi 上线后调用：确保网页监听绑定到新网络接口，管理页经设备 IP 可达。
void prov_web_refresh() {
  if (s_web_running) web_restart();
  else web_start();
}

void prov_loop() {
  if (s_active) s_dns.processNextRequest();
  s_web.handleClient();
}
