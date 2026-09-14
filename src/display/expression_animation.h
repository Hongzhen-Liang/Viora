#pragma once

#include <stddef.h>
#include <stdint.h>

enum class DisplayVisualState : uint8_t {
  kIdle, kSensing, kListening, kThinking, kSpeaking,
};

namespace orchid_animation {
// Half-open regions in the shared 230x210 bitmap canvas. The reference
// supplies all static petals/stems, preventing generative texture shimmer.
static constexpr uint16_t kFaceRegion[] = {40, 85, 146, 146};
static constexpr uint16_t kSleepRegion[] = {180, 18, 225, 74};

inline bool inside(const uint16_t (&region)[4], uint16_t x, uint16_t y) {
  return x >= region[0] && y >= region[1] && x < region[2] && y < region[3];
}

struct Frame {
  uint8_t image;
  uint16_t duration_ms;
};

// Image indices follow assets/1..13. Long rests and brief blinks avoid
// making closed eyes look like sleep; sensing settles instead of restarting.
static constexpr Frame kIdle[] = {
    {0, 3800}, {1, 160}, {0, 2600}, {2, 1100},
    {0, 3200}, {1, 160}, {0, 1800}};
static constexpr Frame kSensing[] = {{3, 260}, {4, 420}, {5, 1600}};
static constexpr Frame kListening[] = {{6, 4200}, {7, 160}, {6, 2300}};
static constexpr Frame kThinking[] = {{8, 1400}, {9, 850}, {8, 1900}};
static constexpr Frame kSpeaking[] = {{10, 520}, {11, 340}, {10, 700}, {11, 420}};

template <size_t N>
inline uint8_t frameAt(const Frame (&frames)[N], uint32_t elapsed,
                       bool repeat = true) {
  uint32_t total = 0;
  for (const Frame &frame : frames) total += frame.duration_ms;
  if (!repeat && elapsed >= total) return frames[N - 1].image;
  elapsed %= total;
  for (const Frame &frame : frames) {
    if (elapsed < frame.duration_ms) return frame.image;
    elapsed -= frame.duration_ms;
  }
  return frames[N - 1].image;
}

inline uint8_t imageAt(DisplayVisualState state, uint32_t elapsed) {
  switch (state) {
    case DisplayVisualState::kSensing: return frameAt(kSensing, elapsed, false);
    case DisplayVisualState::kListening: return frameAt(kListening, elapsed);
    case DisplayVisualState::kThinking: return frameAt(kThinking, elapsed);
    case DisplayVisualState::kSpeaking: return frameAt(kSpeaking, elapsed);
    default: return frameAt(kIdle, elapsed);
  }
}
}  // namespace orchid_animation
