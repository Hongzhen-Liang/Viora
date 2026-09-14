#include <assert.h>
#include <stdint.h>
#include "display/expression_animation.h"

int main() {
  using orchid_animation::imageAt;
  using State = DisplayVisualState;
  // A blink closes briefly then reopens, including at the loop boundary.
  assert(imageAt(State::kIdle, 3799) == 0);
  assert(imageAt(State::kIdle, 3800) == 1);
  assert(imageAt(State::kIdle, 3959) == 1);
  assert(imageAt(State::kIdle, 3960) == 0);
  assert(imageAt(State::kIdle, 12820) == 0);
  assert(imageAt(State::kListening, 4200) == 7);
  assert(imageAt(State::kListening, 4360) == 6);
  // Sensing is a one-shot entrance that remains attentive indefinitely.
  assert(imageAt(State::kSensing, 0) == 3);
  assert(imageAt(State::kSensing, 260) == 4);
  assert(imageAt(State::kSensing, 680) == 5);
  assert(imageAt(State::kSensing, UINT32_MAX) == 5);
  // State-relative unsigned elapsed time remains valid across millis wrap.
  const uint32_t started = UINT32_MAX - 99;
  assert(imageAt(State::kSensing, uint32_t(200 - started)) == 4);
  for (uint8_t state = 0; state < 5; ++state) {
    for (uint32_t time = 0; time < 30000; ++time) {
      assert(imageAt(static_cast<State>(state), time) < 12);
    }
  }
}
