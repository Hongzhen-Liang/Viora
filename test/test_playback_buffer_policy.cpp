#include "../src/audio/playback_buffer_policy.h"
#include <assert.h>

int main() {
  PlaybackBufferPolicy policy;
  assert(policy.targetMs() == 512);
  policy.begin(true); policy.starved(); policy.starved(); policy.finish();
  assert(policy.targetMs() == 768); // one poor answer raises once
  policy.begin(false); policy.finish();
  assert(policy.targetMs() == 768); // local ack doesn't count as recovery
  for (int i = 0; i < 2; ++i) { policy.begin(true); policy.finish(); }
  assert(policy.targetMs() == 768);
  policy.begin(true); // interrupted: no finish
  policy.begin(true); policy.finish();
  assert(policy.targetMs() == 640);
  for (int i = 0; i < 30; ++i) { policy.begin(true); policy.starved(); policy.finish(); }
  assert(policy.targetMs() == 1024);
  for (int i = 0; i < 30; ++i) { policy.begin(true); policy.finish(); }
  assert(policy.targetMs() == 256);
  assert(!playback_buffer_ready(0, true, false, 16384));
  assert(!playback_buffer_ready(8192, false, false, 16384));
  assert(playback_buffer_ready(16384, false, false, 16384));
  assert(playback_buffer_ready(1024, true, false, 16384)); // short completed reply
  assert(playback_buffer_ready(1024, false, true, 16384)); // uninterrupted playback
  assert(playback_buffer_ready(1024, false, false, 0)); // local acknowledgement
}
