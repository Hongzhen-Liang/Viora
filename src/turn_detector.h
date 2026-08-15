#pragma once

#include <stdint.h>

// Pure state machine for deciding when a spoken turn starts and ends.  It has
// no Arduino/ESP-SR dependency, so the timing policy can be host-tested.
enum TurnEvent : uint8_t {
  TURN_EVENT_NONE = 0,
  TURN_EVENT_SPEECH_STARTED,
  TURN_EVENT_SHORT_NOISE,
  TURN_EVENT_ENDPOINT,
  TURN_EVENT_IDLE_TIMEOUT,
};

struct TurnDetectorConfig {
  uint32_t frame_ms;
  uint16_t speech_start_frames;
  uint16_t min_voice_frames;
  uint32_t min_record_ms;
  uint32_t max_record_ms;
  uint32_t idle_timeout_ms;
  uint32_t short_turn_speech_ms;
  uint32_t short_turn_silence_ms;
  uint32_t normal_silence_ms;
  uint32_t long_turn_ms;
  uint32_t long_turn_silence_ms;
  uint32_t max_silence_ms;
  uint32_t min_learned_gap_ms;
};

class TurnDetector {
 public:
  explicit TurnDetector(const TurnDetectorConfig &config);

  // guard_ms ignores speech decisions for a short speaker-tail settling time.
  void reset(uint32_t now_ms, uint32_t guard_ms = 0);
  // A caller such as barge-in may already have confirmed several speech frames
  // before switching into the listening state. Preserve that evidence.
  void prime_speech(uint32_t now_ms, uint16_t prior_voice_frames);
  TurnEvent update(uint32_t now_ms, bool is_speech);

  bool speech_started() const { return speech_started_; }
  uint32_t listen_start_ms() const { return listen_start_ms_; }
  uint32_t speech_start_ms() const { return speech_start_ms_; }
  uint32_t last_voice_ms() const { return last_voice_ms_; }
  uint32_t voice_frames() const { return voice_frames_; }
  uint32_t max_completed_gap_ms() const { return max_completed_gap_ms_; }
  uint32_t endpoint_silence_ms() const;
  uint32_t current_silence_ms(uint32_t now_ms) const;

 private:
  void reset_candidate();
  void reject_short_noise();

  TurnDetectorConfig config_;
  uint32_t listen_start_ms_ = 0;
  uint32_t guard_until_ms_ = 0;
  uint32_t candidate_start_ms_ = 0;
  uint32_t speech_start_ms_ = 0;
  uint32_t last_voice_ms_ = 0;
  uint32_t max_completed_gap_ms_ = 0;
  uint32_t voice_frames_ = 0;
  uint16_t consecutive_voice_frames_ = 0;
  bool speech_started_ = false;
  bool in_gap_ = false;
  bool ignore_gap_until_live_speech_ = false;
};
