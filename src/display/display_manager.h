#pragma once

#include <Arduino.h>

enum class DisplayNetworkState : uint8_t {
  kOffline,
  kProvisioning,
  kServiceConnecting,
  kOnline,
};

class DisplayManager {
 public:
  bool begin();
  void showIdleDashboard(float temperature, float humidity, float soil,
                         DisplayNetworkState network_state);
  void setSubtitle(const char *text);
  void startSpeaking();
  void beginTimedSubtitles(const char *text);
  void queueTimedSubtitle(const char *text, uint32_t pcm_offset_bytes);
  void loop(bool speaking, uint32_t playback_position_bytes);

 private:
  static constexpr uint8_t kMaxLines = 48;
  static constexpr uint8_t kLinesPerPage = 2;

  void wrapSubtitle(const char *text);
  void renderPage();
  void renderIdleDashboard();
  uint32_t currentPageDurationMs() const;

  struct TimedCue {
    String text;
    uint32_t pcm_offset_bytes = 0;
  };
  static constexpr uint8_t kMaxTimedCues = 16;

  bool ready_ = false;
  bool idle_mode_ = false;
  float idle_temperature_ = NAN;
  float idle_humidity_ = NAN;
  float idle_soil_ = NAN;
  bool idle_clock_ready_ = false;
  DisplayNetworkState idle_network_state_ = DisplayNetworkState::kOffline;
  uint32_t last_idle_render_ms_ = 0;
  String lines_[kMaxLines];
  uint8_t line_count_ = 0;
  uint8_t page_ = 0;
  uint8_t page_count_ = 1;
  uint32_t last_page_ms_ = 0;
  bool timed_mode_ = false;
  TimedCue timed_cues_[kMaxTimedCues];
  uint8_t timed_cue_count_ = 0;
};

extern DisplayManager g_display;
