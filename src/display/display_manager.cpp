#include "display/display_manager.h"

#include <U8g2lib.h>
#include <cmath>
#include <ctime>
#include <qrcode.h>

#include "config.h"
#include "display/orchid_bitmap.h"
#include "hardware/hardware_config.h"

namespace {

constexpr uint16_t kScreenWidth = 400;
constexpr uint16_t kScreenHeight = 300;
constexpr uint16_t kSubtitleWidth = 360;
constexpr uint16_t kSubtitleTop = 233;
constexpr uint32_t kSubtitleMsPerCharacter = 260;
constexpr uint32_t kSubtitlePageMinMs = 4800;
constexpr uint32_t kSubtitlePageMaxMs = 12000;
constexpr uint32_t kIdleRefreshMs = 60000;
constexpr uint32_t kUnsyncedClockRefreshMs = 10000;
constexpr uint32_t kBindingQrDurationMs = 120000;
constexpr uint8_t kBindingQrScale = 3;

// 19x14 的紧凑状态栏 Wi-Fi 图标。时间是待机页的主信息，网络图标
// 只作为辅助状态，因此刻意缩小一级，避免与时间争夺视觉注意力。
constexpr uint8_t kWifiIconWidth = 19;
constexpr uint8_t kWifiIconHeight = 14;
const uint8_t kWifiIcon[] PROGMEM = {
    0x00, 0x07, 0x00, 0xf0, 0x7f, 0x00, 0xfc, 0xff, 0x01,
    0x7e, 0xf0, 0x03, 0x0f, 0x80, 0x07, 0x07, 0x02, 0x07,
    0xe0, 0x3f, 0x00, 0xf0, 0x7f, 0x00, 0xf8, 0xf8, 0x00,
    0x38, 0xe0, 0x00, 0x80, 0x0f, 0x00, 0x80, 0x0f, 0x00,
    0x00, 0x07, 0x00, 0x00, 0x02, 0x00,
};

// 16x16 的线稿人物图标。使用固定点阵可避免部分 ST7305/U8g2 组合
// 在绘制实心圆角矩形时出现横向异常填充。
constexpr uint8_t kPresenceIconWidth = 16;
constexpr uint8_t kPresenceIconHeight = 16;
const uint8_t kPresenceIcon[] PROGMEM = {
    0xc0, 0x03, 0x20, 0x04, 0x10, 0x08, 0x10, 0x08,
    0x20, 0x04, 0xc0, 0x03, 0x80, 0x01, 0xe0, 0x07,
    0x18, 0x18, 0x04, 0x20, 0x02, 0x40, 0x02, 0x40,
    0x02, 0x40, 0x04, 0x20, 0xf8, 0x1f, 0x00, 0x00,
};

// Software SPI keeps this display independent from future TF-card use.  The
// ST7305 reflective LCD only transfers 15 KB for a full refresh, so updates
// remain quick and happen outside the real-time audio task.
U8G2_ST7305_300X400_F_4W_SW_SPI s_lcd(
    U8G2_R1, RLCD_SCK_PIN, RLCD_MOSI_PIN, RLCD_CS_PIN, RLCD_DC_PIN,
    RLCD_RST_PIN);

char s_binding_qr_pairing_code[13] = {};

void render_qr_callback(esp_qrcode_handle_t qrcode) {
  const int modules = esp_qrcode_get_size(qrcode);
  if (modules <= 0 || modules > 177) return;

  const int quiet_zone = 4 * kBindingQrScale;
  const int qr_pixels = modules * kBindingQrScale;
  const int total_pixels = qr_pixels + quiet_zone * 2;
  const int x = (kScreenWidth - total_pixels) / 2;
  const int y = 29;

  s_lcd.clearBuffer();
  s_lcd.setFont(u8g2_font_wqy16_t_gb2312);
  const char *title = "扫描二维码绑定";
  const uint16_t title_width = s_lcd.getUTF8Width(title);
  s_lcd.drawUTF8((kScreenWidth - title_width) / 2, 18, title);
  s_lcd.setDrawColor(0);
  s_lcd.drawBox(x, y, total_pixels, total_pixels);
  s_lcd.setDrawColor(1);
  for (int row = 0; row < modules; ++row) {
    for (int col = 0; col < modules; ++col) {
      if (esp_qrcode_get_module(qrcode, col, row)) {
        s_lcd.drawBox(x + quiet_zone + col * kBindingQrScale,
                      y + quiet_zone + row * kBindingQrScale,
                      kBindingQrScale, kBindingQrScale);
      }
    }
  }
  s_lcd.setFont(u8g2_font_wqy16_t_gb2312);
  char pairing_line[40] = {};
  snprintf(pairing_line, sizeof(pairing_line), "配对码 %s",
           s_binding_qr_pairing_code);
  const uint16_t pairing_width = s_lcd.getUTF8Width(pairing_line);
  s_lcd.drawUTF8((kScreenWidth - pairing_width) / 2, 266, pairing_line);
  const char *hint = "扫描后登录即可绑定";
  const uint16_t hint_width = s_lcd.getUTF8Width(hint);
  s_lcd.drawUTF8((kScreenWidth - hint_width) / 2, 291, hint);
  s_lcd.sendBuffer();
}

size_t utf8CharBytes(uint8_t lead) {
  if ((lead & 0x80U) == 0) return 1;
  if ((lead & 0xE0U) == 0xC0U) return 2;
  if ((lead & 0xF0U) == 0xE0U) return 3;
  if ((lead & 0xF8U) == 0xF0U) return 4;
  return 1;
}

}  // namespace

DisplayManager g_display;

bool DisplayManager::begin() {
  s_lcd.begin();
  s_lcd.setBusClock(10000000);
  s_lcd.setFontMode(1);
  s_lcd.setDrawColor(1);
  ready_ = true;
  setSubtitle("你好，我是小芯。");
  Serial.printf("[DISPLAY] ST7305 就绪: %ux%u，蝴蝶兰角色已加载\n",
                kScreenWidth, kScreenHeight);
  return true;
}

void DisplayManager::showIdleDashboard(float temperature, float humidity,
                                       float soil,
                                       DisplayNetworkState network_state) {
  if (!ready_) return;
  if (binding_qr_active_) return;
  // 初始化后的第一次有效读数必须立即上屏，不能被一分钟的常规刷新节流
  // 挡住。否则串口已经有土壤百分比，屏幕仍会暂时显示“未连接”。
  const bool availability_changed =
      (std::isnan(idle_temperature_) != std::isnan(temperature)) ||
      (std::isnan(idle_humidity_) != std::isnan(humidity)) ||
      (std::isnan(idle_soil_) != std::isnan(soil));
  const bool network_state_changed = network_state != idle_network_state_;
  idle_temperature_ = temperature;
  idle_humidity_ = humidity;
  idle_soil_ = soil;
  idle_network_state_ = network_state;

  const uint32_t now = millis();
  const bool clock_ready = time(nullptr) >= 1577836800;
  const bool clock_became_ready = clock_ready && !idle_clock_ready_;
  idle_clock_ready_ = clock_ready;
  if (clock_became_ready) {
    const time_t china_time = time(nullptr) + DEVICE_UTC_OFFSET_SECONDS;
    struct tm china_tm;
    gmtime_r(&china_time, &china_tm);
    char synced_time[24];
    strftime(synced_time, sizeof(synced_time), "%Y-%m-%d %H:%M:%S", &china_tm);
    Serial.printf("[TIME] 校时完成，北京时间 %s\n", synced_time);
  }
  const uint32_t refresh_ms =
      clock_ready ? kIdleRefreshMs : kUnsyncedClockRefreshMs;
  if (idle_mode_ && !availability_changed && !network_state_changed &&
      !clock_became_ready && now - last_idle_render_ms_ < refresh_ms) {
    return;
  }
  idle_mode_ = true;
  timed_mode_ = false;
  timed_cue_count_ = 0;
  last_idle_render_ms_ = now;
  renderIdleDashboard();
}

void DisplayManager::setUpdateAvailable(bool available) {
  if (!ready_ || update_available_ == available) return;
  update_available_ = available;
  if (idle_mode_) {
    last_idle_render_ms_ = millis();
    renderIdleDashboard();
  }
}

void DisplayManager::setPresence(bool present) {
  if (!ready_) return;
  idle_presence_ = present;
  const bool presence_changed = idle_presence_ != rendered_presence_;
  if (idle_mode_ && presence_changed) renderIdleDashboard();
}

void DisplayManager::showBindingQr(const char *url, const char *pairing_code) {
  if (!ready_ || url == nullptr || url[0] == '\0') return;
  idle_mode_ = false;
  timed_mode_ = false;
  timed_cue_count_ = 0;
  binding_qr_active_ = true;
  binding_qr_deadline_ms_ = millis() + kBindingQrDurationMs;
  strlcpy(s_binding_qr_pairing_code, pairing_code ? pairing_code : "",
          sizeof(s_binding_qr_pairing_code));

  esp_qrcode_config_t config = ESP_QRCODE_CONFIG_DEFAULT();
  config.display_func = render_qr_callback;
  config.max_qrcode_version = 10;
  config.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  const esp_err_t result = esp_qrcode_generate(&config, url);
  if (result != ESP_OK) {
    binding_qr_active_ = false;
    binding_qr_deadline_ms_ = 0;
    Serial.printf("[DISPLAY] 二维码生成失败: %s\n", esp_err_to_name(result));
    setSubtitle("二维码生成失败，请使用网页手动绑定");
    return;
  }
  Serial.println("[DISPLAY] 已显示绑定二维码（120 秒后返回待机）");
}

void DisplayManager::showOtaScreen(const char *line1, const char *line2) {
  String text = line1 ? line1 : "";
  if (line2 && line2[0] != '\0') {
    text += '\n';
    text += line2;
  }
  setSubtitle(text.c_str());
}

void DisplayManager::wrapSubtitle(const char *text) {
  line_count_ = 0;
  if (text == nullptr || text[0] == '\0') return;

  s_lcd.setFont(u8g2_font_wqy16_t_gb2312);
  String line;
  const uint8_t *cursor = reinterpret_cast<const uint8_t *>(text);
  while (*cursor != 0 && line_count_ < kMaxLines) {
    if (*cursor == '\r') {
      ++cursor;
      continue;
    }
    if (*cursor == '\n') {
      lines_[line_count_++] = line;
      line = "";
      ++cursor;
      continue;
    }

    size_t bytes = utf8CharBytes(*cursor);
    size_t available = 0;
    while (cursor[available] != 0 && available < bytes) ++available;
    if (available < bytes) bytes = available;
    String character;
    for (size_t i = 0; i < bytes; ++i) {
      character += static_cast<char>(cursor[i]);
    }

    String candidate = line + character;
    if (line.length() > 0 &&
        s_lcd.getUTF8Width(candidate.c_str()) > kSubtitleWidth) {
      lines_[line_count_++] = line;
      line = character;
    } else {
      line = candidate;
    }
    cursor += bytes;
  }
  if (line.length() > 0 && line_count_ < kMaxLines) {
    lines_[line_count_++] = line;
  }
}

void DisplayManager::setSubtitle(const char *text) {
  if (!ready_) return;
  binding_qr_active_ = false;
  binding_qr_deadline_ms_ = 0;
  idle_mode_ = false;
  timed_mode_ = false;
  timed_cue_count_ = 0;
  wrapSubtitle(text);
  page_ = 0;
  page_count_ = line_count_ == 0
                    ? 1
                    : (line_count_ + kLinesPerPage - 1) / kLinesPerPage;
  last_page_ms_ = millis();
  renderPage();
}

void DisplayManager::beginTimedSubtitles(const char *text) {
  if (!ready_) return;
  idle_mode_ = false;
  timed_mode_ = true;
  timed_cue_count_ = 0;
  wrapSubtitle(text);
  page_ = 0;
  page_count_ = line_count_ == 0
                    ? 1
                    : (line_count_ + kLinesPerPage - 1) / kLinesPerPage;
  renderPage();
}

void DisplayManager::queueTimedSubtitle(const char *text,
                                        uint32_t pcm_offset_bytes) {
  if (!ready_ || text == nullptr || text[0] == '\0') return;
  if (!timed_mode_) timed_mode_ = true;
  if (timed_cue_count_ >= kMaxTimedCues) {
    Serial.println("[DISPLAY] 字幕时间轴过长，已忽略末尾 cue");
    return;
  }
  timed_cues_[timed_cue_count_].text = text;
  timed_cues_[timed_cue_count_].pcm_offset_bytes = pcm_offset_bytes;
  ++timed_cue_count_;
}

void DisplayManager::startSpeaking() {
  if (!ready_) return;
  // 文字通常比 TTS 音频更早到达。翻页时钟必须从扬声器这一轮
  // 真正开始播放时重置，不能沿用收到文字时的旧时间。
  page_ = 0;
  last_page_ms_ = millis();
  renderPage();
}

uint32_t DisplayManager::currentPageDurationMs() const {
  const uint8_t first = page_ * kLinesPerPage;
  const uint8_t remaining = line_count_ > first ? line_count_ - first : 0;
  const uint8_t visible_lines =
      remaining < kLinesPerPage ? remaining : kLinesPerPage;
  uint16_t characters = 0;
  for (uint8_t row = 0; row < visible_lines; ++row) {
    const uint8_t *cursor = reinterpret_cast<const uint8_t *>(
        lines_[first + row].c_str());
    while (*cursor != 0) {
      const size_t bytes = utf8CharBytes(*cursor);
      size_t available = 0;
      while (cursor[available] != 0 && available < bytes) ++available;
      cursor += available > 0 ? available : 1;
      ++characters;
    }
  }
  uint32_t duration =
      static_cast<uint32_t>(characters) * kSubtitleMsPerCharacter;
  if (duration < kSubtitlePageMinMs) duration = kSubtitlePageMinMs;
  if (duration > kSubtitlePageMaxMs) duration = kSubtitlePageMaxMs;
  return duration;
}

void DisplayManager::loop(bool speaking, uint32_t playback_position_bytes) {
  if (!ready_) return;
  if (binding_qr_active_) {
    if (static_cast<int32_t>(millis() - binding_qr_deadline_ms_) >= 0) {
      binding_qr_active_ = false;
      binding_qr_deadline_ms_ = 0;
      renderBindingQrExpired();
    }
    return;
  }
  if (!speaking) return;

  if (timed_mode_) {
    while (timed_cue_count_ > 0 &&
           playback_position_bytes >= timed_cues_[0].pcm_offset_bytes) {
      String next = timed_cues_[0].text;
      for (uint8_t i = 1; i < timed_cue_count_; ++i) {
        timed_cues_[i - 1] = timed_cues_[i];
      }
      --timed_cue_count_;
      wrapSubtitle(next.c_str());
      page_ = 0;
      page_count_ = line_count_ == 0
                        ? 1
                        : (line_count_ + kLinesPerPage - 1) / kLinesPerPage;
      renderPage();
    }
    return;
  }

  if (page_count_ <= 1) return;
  const uint32_t now = millis();
  if (now - last_page_ms_ < currentPageDurationMs()) return;
  last_page_ms_ = now;
  if (page_ + 1 < page_count_) {
    ++page_;
    renderPage();
  }
}

void DisplayManager::renderBindingQrExpired() {
  idle_mode_ = true;
  last_idle_render_ms_ = millis();
  renderIdleDashboard();
}

void DisplayManager::renderPage() {
  s_lcd.clearBuffer();

  const uint16_t image_x = (kScreenWidth - ORCHID_BITMAP_WIDTH) / 2;
  s_lcd.drawXBMP(image_x, 0, ORCHID_BITMAP_WIDTH, ORCHID_BITMAP_HEIGHT,
                 ORCHID_BITMAP);

  // 一条带中央菱形的细分隔线，比标题、边框和页码更克制，也与
  // 上方植物线稿的气质一致。
  s_lcd.drawHLine(24, kSubtitleTop, 158);
  s_lcd.drawHLine(218, kSubtitleTop, 158);
  s_lcd.drawLine(194, kSubtitleTop, 200, kSubtitleTop - 4);
  s_lcd.drawLine(200, kSubtitleTop - 4, 206, kSubtitleTop);
  s_lcd.drawLine(206, kSubtitleTop, 200, kSubtitleTop + 4);
  s_lcd.drawLine(200, kSubtitleTop + 4, 194, kSubtitleTop);

  s_lcd.setFont(u8g2_font_wqy16_t_gb2312);
  const uint8_t first = page_ * kLinesPerPage;
  const uint8_t remaining = line_count_ > first ? line_count_ - first : 0;
  const uint8_t visible_lines =
      remaining < kLinesPerPage ? remaining : kLinesPerPage;
  const uint16_t first_baseline = visible_lines <= 1 ? 271 : 255;
  for (uint8_t row = 0; row < visible_lines; ++row) {
    const uint8_t line = first + row;
    const uint16_t text_width = s_lcd.getUTF8Width(lines_[line].c_str());
    const uint16_t x =
        text_width < kScreenWidth ? (kScreenWidth - text_width) / 2 : 0;
    s_lcd.drawUTF8(x, first_baseline + row * 29, lines_[line].c_str());
  }
  s_lcd.sendBuffer();
}

void DisplayManager::renderIdleDashboard() {
  s_lcd.clearBuffer();

  const uint16_t image_x = (kScreenWidth - ORCHID_BITMAP_WIDTH) / 2;
  s_lcd.drawXBMP(image_x, 0, ORCHID_BITMAP_WIDTH, ORCHID_BITMAP_HEIGHT,
                 ORCHID_BITMAP);

  // 时间占用植物线稿左侧的留白，不挤压角色主体。NTP 尚未校准时
  // 明确显示占位符，避免把 1970 年的系统初始值误当成真实时间。
  char time_text[6] = "--:--";
  char date_text[24] = "";
  const time_t now = time(nullptr);
  if (now >= 1577836800) {  // 2020-01-01，早于此值视为尚未校时
    // 系统 epoch 始终按 UTC 保存；显示时显式加 8 小时，再用 gmtime_r
    // 拆分。这样不依赖 C 运行库的 TZ 状态，也绝不会套用夏令时。
    const time_t china_time = now + DEVICE_UTC_OFFSET_SECONDS;
    struct tm china_tm;
    gmtime_r(&china_time, &china_tm);
    strftime(time_text, sizeof(time_text), "%H:%M", &china_tm);
    // 使用“周四 8/27”这种紧凑中文格式。它与英文短日期宽度接近，
    // 并可复用界面已有的中文字库，避免为小字号字库额外占用 Flash。
    static const char *const kWeekdays[] = {
        "周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    snprintf(date_text, sizeof(date_text), "%s %d/%d",
             kWeekdays[china_tm.tm_wday], china_tm.tm_mon + 1,
             china_tm.tm_mday);
  }
  s_lcd.setFont(u8g2_font_helvB12_tf);
  s_lcd.drawStr(5, 20, time_text);
  if (date_text[0] != '\0') {
    const uint16_t date_x = 5 + s_lcd.getStrWidth(time_text) + 9;
    s_lcd.setFont(u8g2_font_wqy16_t_gb2312);
    s_lcd.drawUTF8(date_x, 19, date_text);
  }

  // 右上角显示“整机是否可用”，而不只是 Wi-Fi 关联状态。
  // 正常时为克制的 Wi-Fi 图标；服务端未连时加 !；离线时加斜线；
  // 配网模式直接显示“配网”，让用户知道下一步该做什么。
  if (idle_network_state_ == DisplayNetworkState::kProvisioning) {
    s_lcd.setFont(u8g2_font_wqy16_t_gb2312);
    s_lcd.drawUTF8(363, 20, "配网");
  } else {
    constexpr uint16_t wifi_x = 380;
    constexpr uint16_t wifi_y = 6;
    s_lcd.drawXBMP(wifi_x, wifi_y, kWifiIconWidth, kWifiIconHeight,
                   kWifiIcon);

    if (idle_network_state_ == DisplayNetworkState::kOffline) {
      s_lcd.drawLine(379, 6, 399, 20);
      s_lcd.drawLine(380, 6, 399, 19);
    } else if (idle_network_state_ ==
               DisplayNetworkState::kServiceConnecting) {
      s_lcd.setFont(u8g2_font_helvB12_tf);
      s_lcd.drawStr(356, 21, "!");
    }
  }

  // 有新版本或已有固件待安装时，在 Wi-Fi 左侧显示“向下箭头进入托盘”。
  // 图标完全由矢量线绘制，不占额外位图空间。
  if (update_available_) {
    // 人物图标出现时给它让位，保持右侧状态图标紧凑且不重叠。
    const uint16_t icon_x = idle_presence_ ? 333 : 342;
    constexpr uint16_t icon_y = 5;
    s_lcd.drawRFrame(icon_x, icon_y, 24, 18, 4);
    s_lcd.drawVLine(icon_x + 12, icon_y + 3, 7);
    s_lcd.drawLine(icon_x + 8, icon_y + 7, icon_x + 12, icon_y + 11);
    s_lcd.drawLine(icon_x + 16, icon_y + 7, icon_x + 12, icon_y + 11);
    s_lcd.drawHLine(icon_x + 7, icon_y + 14, 11);
  }

  // 人在图标：仅显示小型线稿人物，不显示文字或距离。
  if (idle_presence_) {
    s_lcd.drawXBMP(363, 4, kPresenceIconWidth, kPresenceIconHeight,
                   kPresenceIcon);
  }

  rendered_presence_ = idle_presence_;

  s_lcd.drawHLine(24, kSubtitleTop, 352);

  char soil_value[12] = "--";
  const char *soil_state = "传感器未连接";
  if (!std::isnan(idle_soil_)) {
    snprintf(soil_value, sizeof(soil_value), "%.0f%%", idle_soil_);
    if (idle_soil_ < 30.0f) {
      soil_state = "偏干";
    } else if (idle_soil_ <= 75.0f) {
      soil_state = "适宜";
    } else {
      soil_state = "偏湿";
    }
  }

  char temperature[16] = "--";
  char humidity[16] = "--";
  if (!std::isnan(idle_temperature_)) {
    snprintf(temperature, sizeof(temperature), "%.1f°C", idle_temperature_);
  }
  if (!std::isnan(idle_humidity_)) {
    snprintf(humidity, sizeof(humidity), "%.0f%%", idle_humidity_);
  }

  s_lcd.setFont(u8g2_font_wqy16_t_gb2312);
  String soil_line = String("土壤湿度 ") + soil_value + "  " + soil_state;
  uint16_t width = s_lcd.getUTF8Width(soil_line.c_str());
  s_lcd.drawUTF8(width < kScreenWidth ? (kScreenWidth - width) / 2 : 0, 257,
                 soil_line.c_str());

  String environment_line =
      String("温度 ") + temperature + "  空气湿度 " + humidity;
  width = s_lcd.getUTF8Width(environment_line.c_str());
  s_lcd.drawUTF8(width < kScreenWidth ? (kScreenWidth - width) / 2 : 0, 286,
                 environment_line.c_str());
  s_lcd.sendBuffer();
}
