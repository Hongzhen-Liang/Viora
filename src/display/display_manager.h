#pragma once

#include <Arduino.h>

class DisplayManager {
 public:
  bool begin();
  void setSubtitle(const char *text);
  void loop(bool speaking);

 private:
  static constexpr uint8_t kMaxLines = 48;
  static constexpr uint8_t kLinesPerPage = 2;

  void wrapSubtitle(const char *text);
  void renderPage();

  bool ready_ = false;
  String lines_[kMaxLines];
  uint8_t line_count_ = 0;
  uint8_t page_ = 0;
  uint8_t page_count_ = 1;
  uint32_t last_page_ms_ = 0;
};

extern DisplayManager g_display;
