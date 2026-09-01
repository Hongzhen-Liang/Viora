#include "turn_detector.h"

namespace {

uint32_t max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }
uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

}  // namespace

bool elapsed_at_least(uint32_t now_ms, uint32_t since_ms,
                      uint32_t duration_ms) {
  return now_ms - since_ms >= duration_ms;
}

const char *listen_source_for(ListenOrigin origin) {
  if (origin == LISTEN_FROM_WAKE) return "wake";
  if (origin == LISTEN_FROM_BARGE_IN) return "barge_in";
  // LISTEN_FROM_WAKE_ACK has already played the acknowledgement locally; its
  // post-ack audio is semantically a follow-up even though it starts a new
  // conversation context.
  return "follow_up";
}

bool listen_starts_new_conversation(ListenOrigin origin) {
  return origin == LISTEN_FROM_WAKE || origin == LISTEN_FROM_WAKE_ACK;
}

SpeechEvidenceGate::SpeechEvidenceGate(const SpeechEvidenceConfig &config)
    : config_(config) {}

void SpeechEvidenceGate::reset() {
  missing_neural_frames_ = 0;
  consecutive_energy_frames_ = 0;
  last_neural_speech_ = false;
}

bool SpeechEvidenceGate::update(bool neural_available, bool neural_speech,
                                bool energy_speech) {
  if (neural_available) {
    missing_neural_frames_ = 0;
    consecutive_energy_frames_ = 0;
    last_neural_speech_ = neural_speech;
    return neural_speech;
  }

  if (missing_neural_frames_ != UINT16_MAX) ++missing_neural_frames_;
  if (energy_speech) {
    if (consecutive_energy_frames_ != UINT16_MAX) {
      ++consecutive_energy_frames_;
    }
  } else {
    consecutive_energy_frames_ = 0;
  }

  if (last_neural_speech_ &&
      missing_neural_frames_ <= config_.neural_hold_frames) {
    return true;
  }
  last_neural_speech_ = false;

  return config_.energy_fallback_frames > 0 &&
         consecutive_energy_frames_ >= config_.energy_fallback_frames;
}

WakeAckGate::WakeAckGate(const WakeAckGateConfig &config) : config_(config) {}

void WakeAckGate::reset(uint32_t now_ms) {
  start_ms_ = now_ms;
  quiet_frames_ = 0;
  voice_frames_ = 0;
  tail_released_ = false;
}

bool WakeAckGate::update(uint32_t now_ms, bool is_speech,
                         bool trusted_quiet) {
  if (!is_speech) {
    if (trusted_quiet) {
      if (quiet_frames_ != UINT16_MAX) ++quiet_frames_;
      if (quiet_frames_ >= config_.quiet_frames) tail_released_ = true;
    } else {
      // AFE unavailable while raw energy is still high means unknown, not a
      // reliable silence boundary after the wake phrase.
      quiet_frames_ = 0;
    }
    voice_frames_ = 0;
    return false;
  }

  const uint32_t guard_ms = tail_released_ ? config_.separated_guard_ms
                                           : config_.continuous_guard_ms;
  if (!elapsed_at_least(now_ms, start_ms_, guard_ms)) {
    voice_frames_ = 0;
    return false;
  }

  if (voice_frames_ != UINT16_MAX) ++voice_frames_;
  const uint16_t required =
      tail_released_ ? config_.separated_voice_frames
                     : config_.continuous_voice_frames;
  return required > 0 && voice_frames_ >= required;
}

TurnDetector::TurnDetector(const TurnDetectorConfig &config)
    : config_(config), idle_timeout_ms_(config.idle_timeout_ms) {}

void TurnDetector::reset(uint32_t now_ms, uint32_t guard_ms) {
  listen_start_ms_ = now_ms;
  guard_until_ms_ = now_ms + guard_ms;
  speech_start_ms_ = 0;
  last_voice_ms_ = now_ms;
  max_completed_gap_ms_ = 0;
  voice_frames_ = 0;
  speech_started_ = false;
  in_gap_ = false;
  ignore_gap_until_live_speech_ = false;
  reset_candidate();
}

void TurnDetector::set_idle_timeout_ms(uint32_t timeout_ms) {
  // Keep corrupted or future protocol values from holding the microphone open
  // forever or making it close before the user can answer.
  if (timeout_ms < 5000) timeout_ms = 5000;
  if (timeout_ms > 30000) timeout_ms = 30000;
  idle_timeout_ms_ = timeout_ms;
}

void TurnDetector::prime_speech(uint32_t now_ms,
                                uint16_t prior_voice_frames) {
  speech_started_ = true;
  const uint32_t prior_ms = prior_voice_frames * config_.frame_ms;
  speech_start_ms_ = now_ms - prior_ms;
  last_voice_ms_ = now_ms;
  voice_frames_ = prior_voice_frames;
  consecutive_voice_frames_ = prior_voice_frames;
  max_completed_gap_ms_ = 0;
  in_gap_ = false;
  // prime 代表唤醒/打断前已经存在语音证据；从 prime 到第一帧现场人声
  // 的等待不属于用户的“句内停顿”，不能拿来放宽后续端点。
  ignore_gap_until_live_speech_ = true;
}

void TurnDetector::reset_candidate() {
  candidate_start_ms_ = 0;
  consecutive_voice_frames_ = 0;
}

void TurnDetector::reject_short_noise() {
  speech_start_ms_ = 0;
  last_voice_ms_ = listen_start_ms_;
  max_completed_gap_ms_ = 0;
  voice_frames_ = 0;
  speech_started_ = false;
  in_gap_ = false;
  ignore_gap_until_live_speech_ = false;
  reset_candidate();
}

uint32_t TurnDetector::endpoint_silence_ms() const {
  const uint32_t voiced_ms = voice_frames_ * config_.frame_ms;
  const uint32_t elapsed_ms = speech_started_
                                  ? last_voice_ms_ - speech_start_ms_
                                  : 0;

  // A very short answer ("嗯", "等等") is also where hesitation is most
  // likely, so be patient.  Once the turn is established, respond quickly.
  uint32_t silence_ms = config_.normal_silence_ms;
  if (voiced_ms < config_.short_turn_speech_ms) {
    silence_ms = config_.short_turn_silence_ms;
  } else if (elapsed_ms >= config_.long_turn_ms) {
    silence_ms = config_.long_turn_silence_ms;
  }

  // Learn the user's pace from pauses that they resumed after.  This is a
  // lightweight local approximation of semantic turn detection: slow speakers
  // get more room, while ordinary turns retain the fast base endpoint.
  if (max_completed_gap_ms_ >= config_.min_learned_gap_ms) {
    const uint32_t learned = max_completed_gap_ms_ +
                             max_completed_gap_ms_ / 3 + 160;
    silence_ms = max_u32(silence_ms, learned);
  }
  return min_u32(silence_ms, config_.max_silence_ms);
}

uint32_t TurnDetector::current_silence_ms(uint32_t now_ms) const {
  return speech_started_ ? now_ms - last_voice_ms_ : 0;
}

TurnEvent TurnDetector::update(uint32_t now_ms, bool is_speech) {
  if (!speech_started_) {
    if (static_cast<int32_t>(now_ms - guard_until_ms_) < 0) {
      reset_candidate();
      return TURN_EVENT_NONE;
    }

    if (is_speech) {
      if (consecutive_voice_frames_ == 0) candidate_start_ms_ = now_ms;
      ++consecutive_voice_frames_;
      if (consecutive_voice_frames_ >= config_.speech_start_frames) {
        speech_started_ = true;
        speech_start_ms_ = candidate_start_ms_;
        last_voice_ms_ = now_ms;
        voice_frames_ = consecutive_voice_frames_;
        in_gap_ = false;
        return TURN_EVENT_SPEECH_STARTED;
      }
    } else {
      reset_candidate();
    }

    if (now_ms - listen_start_ms_ >= idle_timeout_ms_) {
      return TURN_EVENT_IDLE_TIMEOUT;
    }
    return TURN_EVENT_NONE;
  }

  if (is_speech) {
    if (ignore_gap_until_live_speech_) {
      ignore_gap_until_live_speech_ = false;
    } else if (in_gap_) {
      const uint32_t gap_ms = now_ms - last_voice_ms_;
      if (gap_ms >= config_.min_learned_gap_ms) {
        max_completed_gap_ms_ = max_u32(max_completed_gap_ms_, gap_ms);
      }
      in_gap_ = false;
    }
    last_voice_ms_ = now_ms;
    ++voice_frames_;
  } else {
    in_gap_ = true;
  }

  const uint32_t record_ms = now_ms - speech_start_ms_;
  const bool silence_endpoint =
      record_ms >= config_.min_record_ms &&
      current_silence_ms(now_ms) >= endpoint_silence_ms();
  const bool hard_endpoint = record_ms >= config_.max_record_ms;
  if (!silence_endpoint && !hard_endpoint) return TURN_EVENT_NONE;

  if (voice_frames_ < config_.min_voice_frames) {
    reject_short_noise();
    return TURN_EVENT_SHORT_NOISE;
  }
  return TURN_EVENT_ENDPOINT;
}
