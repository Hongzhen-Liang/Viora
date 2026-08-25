#include "display/display_manager.h"

#include <U8g2lib.h>

#include "display/orchid_bitmap.h"
#include "hardware/hardware_config.h"

namespace {

constexpr uint16_t kScreenWidth = 400;
constexpr uint16_t kScreenHeight = 300;
constexpr uint16_t kSubtitleLeft = 10;
constexpr uint16_t kSubtitleWidth = 380;
constexpr uint16_t kSubtitleTop = 198;
constexpr uint32_t kSubtitlePageMs = 4200;

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
  setSubtitle("你好，我是小鑫。");
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
  wrapSubtitle(text);
  page_ = 0;
  page_count_ = line_count_ == 0
                    ? 1
                    : (line_count_ + kLinesPerPage - 1) / kLinesPerPage;
  last_page_ms_ = millis();
  renderPage();
}

void DisplayManager::loop(bool speaking) {
  if (!ready_ || !speaking || page_count_ <= 1) return;
  const uint32_t now = millis();
  if (now - last_page_ms_ < kSubtitlePageMs) return;
  last_page_ms_ = now;
  if (page_ + 1 < page_count_) {
    ++page_;
    renderPage();
  }
}

void DisplayManager::renderPage() {
  s_lcd.clearBuffer();

  const uint16_t image_x = (kScreenWidth - ORCHID_BITMAP_WIDTH) / 2;
  s_lcd.drawXBMP(image_x, 1, ORCHID_BITMAP_WIDTH, ORCHID_BITMAP_HEIGHT,
                 ORCHID_BITMAP);
  s_lcd.drawHLine(8, kSubtitleTop, kScreenWidth - 16);

  s_lcd.setFont(u8g2_font_wqy12_t_gb2312);
  s_lcd.drawUTF8(kSubtitleLeft, 212, "小鑫 · 正在说");
  if (page_count_ > 1) {
    char page_label[12];
    snprintf(page_label, sizeof(page_label), "%u/%u", page_ + 1,
             page_count_);
    const uint16_t label_width = s_lcd.getStrWidth(page_label);
    s_lcd.drawStr(kScreenWidth - kSubtitleLeft - label_width, 212,
                  page_label);
  }

  s_lcd.setFont(u8g2_font_wqy16_t_gb2312);
  const uint8_t first = page_ * kLinesPerPage;
  for (uint8_t row = 0; row < kLinesPerPage; ++row) {
    const uint8_t line = first + row;
    if (line >= line_count_) break;
    s_lcd.drawUTF8(kSubtitleLeft, 232 + row * 21, lines_[line].c_str());
  }
  s_lcd.sendBuffer();
}
