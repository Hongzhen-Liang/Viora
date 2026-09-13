#pragma once
#include <stdint.h>

inline bool playback_buffer_ready(uint32_t bytes, bool end_seen,
                                  bool started, uint32_t target) {
  return bytes > 0 && (end_seen || started || bytes >= target);
}

// Caller owns synchronization. Learn only from completed network answers;
// local wake acknowledgements and interrupted answers are not clean samples.
class PlaybackBufferPolicy {
 public:
  uint32_t targetMs() const { return target_ms_; }
  void begin(bool network) { network_ = network; starved_ = false; }
  void starved() {
    if (!network_ || starved_) return;
    starved_ = true;
    clean_ = 0;
    target_ms_ = target_ms_ < 768 ? target_ms_ + 256 : 1024;
  }
  void finish() {
    if (!network_ || starved_) return;
    if (++clean_ >= 3) {
      clean_ = 0;
      if (target_ms_ > 256) target_ms_ -= 128;
    }
  }
 private:
  uint32_t target_ms_ = 512;
  uint8_t clean_ = 0;
  bool network_ = false;
  bool starved_ = false;
};
