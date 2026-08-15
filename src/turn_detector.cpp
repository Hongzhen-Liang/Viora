#include "turn_detector.h"

namespace {

uint32_t max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }
uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

}  // namespace

TurnDetector::TurnDetector(const TurnDetectorConfig &config) : config_(config) {}

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

    if (now_ms - listen_start_ms_ >= config_.idle_timeout_ms) {
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
