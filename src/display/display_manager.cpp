#include "display/display_manager.h"

#include <U8g2lib.h>

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
