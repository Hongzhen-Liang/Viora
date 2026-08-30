#pragma once

#include <Arduino.h>

enum class DisplayNetworkState : uint8_t {
  kOffline,
  kProvisioning,
  kServiceConnecting,
  kOnline,
};

// The expression set is intentionally separate from the conversation state so
// each interaction phase can select the right illustration frame.
enum class DisplayVisualState : uint8_t {
  kIdle,
  kSensing,
  kListening,
  kThinking,
  kSpeaking,
};

class DisplayManager {
 public:
  bool begin();
  void setVisualState(DisplayVisualState state);
  void showIdleDashboard(float temperature, float humidity, float soil,
                         DisplayNetworkState network_state);
  void setPresence(bool present);
  void setUpdateAvailable(bool available);
  void showBindingQr(const char *url, const char *pairing_code);
  bool bindingQrActive() const { return binding_qr_active_; }
  void hideBindingQr();
  void showSettingsMenu(uint8_t selected, bool update_available,
                        bool online);
  void showOtaScreen(const char *line1, const char *line2);
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
  void renderBindingQrExpired();
  uint32_t currentPageDurationMs() const;
  const uint8_t *expressionBitmap() const;
  uint32_t expressionFrameMs() const;

  struct TimedCue {
    String text;
    uint32_t pcm_offset_bytes = 0;
  };
  static constexpr uint8_t kMaxTimedCues = 16;

  bool ready_ = false;
  bool idle_mode_ = false;
  bool update_available_ = false;
  float idle_temperature_ = NAN;
  float idle_humidity_ = NAN;
  float idle_soil_ = NAN;
  bool idle_presence_ = false;
  bool rendered_presence_ = false;
  bool idle_clock_ready_ = false;
  DisplayNetworkState idle_network_state_ = DisplayNetworkState::kOffline;
  uint32_t last_idle_render_ms_ = 0;
  uint32_t last_expression_render_ms_ = 0;
  DisplayVisualState visual_state_ = DisplayVisualState::kIdle;
  bool animated_screen_ = false;
  String lines_[kMaxLines];
  uint8_t line_count_ = 0;
  uint8_t page_ = 0;
  uint8_t page_count_ = 1;
  uint32_t last_page_ms_ = 0;
  bool timed_mode_ = false;
  bool binding_qr_active_ = false;
  uint32_t binding_qr_deadline_ms_ = 0;
  TimedCue timed_cues_[kMaxTimedCues];
  uint8_t timed_cue_count_ = 0;
};

extern DisplayManager g_display;
