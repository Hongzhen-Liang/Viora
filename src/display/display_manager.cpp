#include "display/display_manager.h"

#include <U8g2lib.h>
#include <cmath>
#include <ctime>

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

// Software SPI keeps this display independent from future TF-card use.  The
// ST7305 reflective LCD only transfers 15 KB for a full refresh, so updates
// remain quick and happen outside the real-time audio task.
U8G2_ST7305_300X400_F_4W_SW_SPI s_lcd(
    U8G2_R1, RLCD_SCK_PIN, RLCD_MOSI_PIN, RLCD_CS_PIN, RLCD_DC_PIN,
    RLCD_RST_PIN);

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
  if (!ready_ || !speaking) return;

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
  const time_t now = time(nullptr);
  if (now >= 1577836800) {  // 2020-01-01，早于此值视为尚未校时
    // 系统 epoch 始终按 UTC 保存；显示时显式加 8 小时，再用 gmtime_r
    // 拆分。这样不依赖 C 运行库的 TZ 状态，也绝不会套用夏令时。
    const time_t china_time = now + DEVICE_UTC_OFFSET_SECONDS;
    struct tm china_tm;
    gmtime_r(&china_time, &china_tm);
    strftime(time_text, sizeof(time_text), "%H:%M", &china_tm);
  }
  s_lcd.setFont(u8g2_font_helvB12_tf);
  s_lcd.drawStr(5, 20, time_text);

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
    constexpr uint16_t icon_x = 342;
    constexpr uint16_t icon_y = 5;
    s_lcd.drawRFrame(icon_x, icon_y, 24, 18, 4);
    s_lcd.drawVLine(icon_x + 12, icon_y + 3, 7);
    s_lcd.drawLine(icon_x + 8, icon_y + 7, icon_x + 12, icon_y + 11);
    s_lcd.drawLine(icon_x + 16, icon_y + 7, icon_x + 12, icon_y + 11);
    s_lcd.drawHLine(icon_x + 7, icon_y + 14, 11);
  }

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
