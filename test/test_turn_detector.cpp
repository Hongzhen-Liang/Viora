#include <assert.h>
#include <stdint.h>

#include "../src/turn_detector.h"

static const TurnDetectorConfig kConfig = {
    32, 3, 5, 450, 30000, 15000, 640,
    1200, 850, 5000, 750, 1800, 160,
};

static void establish_normal_turn(TurnDetector &turn, uint32_t *now) {
  for (int i = 0; i < 3; ++i) {
    *now += 32;
    const TurnEvent event = turn.update(*now, true);
    if (i < 2) assert(event == TURN_EVENT_NONE);
    else assert(event == TURN_EVENT_SPEECH_STARTED);
  }
  for (int i = 0; i < 22; ++i) {
    *now += 32;
    assert(turn.update(*now, true) == TURN_EVENT_NONE);
  }
}

int main() {
  {
    TurnDetector turn(kConfig);
    uint32_t now = 0;
    turn.reset(now);
    establish_normal_turn(turn, &now);
    assert(turn.endpoint_silence_ms() == 850);
    TurnEvent event = TURN_EVENT_NONE;
    while (event == TURN_EVENT_NONE) {
      now += 32;
      event = turn.update(now, false);
    }
    assert(event == TURN_EVENT_ENDPOINT);
    assert(turn.current_silence_ms(now) >= 850);
  }

  {
    TurnDetector turn(kConfig);
    uint32_t now = 0;
    turn.reset(now);
    establish_normal_turn(turn, &now);
    // A 704ms pause that the user resumes after teaches a slower cadence.
    for (int i = 0; i < 22; ++i) {
      now += 32;
      assert(turn.update(now, false) == TURN_EVENT_NONE);
    }
    now += 32;
    assert(turn.update(now, true) == TURN_EVENT_NONE);
    assert(turn.max_completed_gap_ms() >= 704);
    assert(turn.endpoint_silence_ms() > 1000);
  }

  {
    TurnDetector turn(kConfig);
    uint32_t now = 0;
    turn.reset(now, 180);
    for (int i = 0; i < 4; ++i) {
      now += 32;
      assert(turn.update(now, true) == TURN_EVENT_NONE);
    }
    while (now < 15000) {
      now += 32;
      const TurnEvent event = turn.update(now, false);
      if (now < 15000) assert(event == TURN_EVENT_NONE);
      else assert(event == TURN_EVENT_IDLE_TIMEOUT);
    }
  }

  {
    TurnDetector turn(kConfig);
    turn.reset(1000);
    turn.prime_speech(1000, 5);
    assert(turn.speech_started());
    assert(turn.voice_frames() == 5);
    assert(turn.speech_start_ms() == 840);
    // 唤醒到第一句话之间的等待不能污染句内节奏学习。
    assert(turn.update(2000, false) == TURN_EVENT_NONE);
    assert(turn.update(2100, true) == TURN_EVENT_NONE);
    assert(turn.max_completed_gap_ms() == 0);
  }

  return 0;
}
