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

enum ListenOrigin : uint8_t {
  LISTEN_FROM_WAKE = 0,
  LISTEN_FROM_WAKE_ACK,
  LISTEN_FROM_FOLLOWUP,
  LISTEN_FROM_BARGE_IN,
};

// A locally acknowledged wake starts a fresh conversation but its following
// audio no longer contains the wake phrase.  Keep these two protocol fields
// independent so the server neither strips a nonexistent prefix nor re-acks.
const char *listen_source_for(ListenOrigin origin);
bool listen_starts_new_conversation(ListenOrigin origin);

// Wrap-safe elapsed-time helper shared by the pure turn policy and the
// firmware's PROCESSING/PLAYING watchdogs.
bool elapsed_at_least(uint32_t now_ms, uint32_t since_ms,
                      uint32_t duration_ms);

struct SpeechEvidenceConfig {
  // Bridge short scheduling gaps after a positive neural-VAD result.
  uint16_t neural_hold_frames;
  // Energy may take over only after AFE produced no result for this many
  // consecutive frames. A fresh neural silence decision always vetoes it.
  uint16_t energy_fallback_frames;
};

// Arbitrates asynchronous neural VAD and the energy-only degraded path.  This
// class deliberately has no Arduino/ESP-SR dependency so background and AFE
// stall behavior can be host-tested.
class SpeechEvidenceGate {
 public:
  explicit SpeechEvidenceGate(const SpeechEvidenceConfig &config);

  void reset();
  bool update(bool neural_available, bool neural_speech,
              bool energy_speech);

 private:
  SpeechEvidenceConfig config_;
  uint16_t missing_neural_frames_ = 0;
  uint16_t consecutive_energy_frames_ = 0;
  bool last_neural_speech_ = false;
};

struct WakeAckGateConfig {
  uint32_t separated_guard_ms;
  uint32_t continuous_guard_ms;
  uint16_t quiet_frames;
  uint16_t separated_voice_frames;
  uint16_t continuous_voice_frames;
};

// Separates post-KWS command speech from the wake phrase's own trailing voice.
// It is pure policy so short-tail, separated-command, and seamless-command
// timing can be checked on the host.
class WakeAckGate {
 public:
  explicit WakeAckGate(const WakeAckGateConfig &config);

  void reset(uint32_t now_ms);
  bool update(uint32_t now_ms, bool is_speech, bool trusted_quiet);
  bool tail_released() const { return tail_released_; }

 private:
  WakeAckGateConfig config_;
  uint32_t start_ms_ = 0;
  uint16_t quiet_frames_ = 0;
  uint16_t voice_frames_ = 0;
  bool tail_released_ = false;
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
  // The server may adapt the open-mic window to the reply: questions deserve
  // more time, while a completed statement can close sooner.
  void set_idle_timeout_ms(uint32_t timeout_ms);
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
  uint32_t idle_timeout_ms_ = 0;
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
