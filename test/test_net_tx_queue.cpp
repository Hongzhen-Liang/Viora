#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <deque>
#include "../src/net_tx_queue.h"

struct Frame { unsigned sequence; bool control; };
int main() {
  // PCM done but audio_end in flight must remain pending. Controls queued
  // after audio_end need not drain. Disconnect/reset/fault overrides success.
  assert(net_tx_fence_status(1, 1, 10, 11, false, true) == NetDrainStatus::Pending);
  assert(net_tx_fence_status(1, 1, 11, 11, false, true) == NetDrainStatus::Complete);
  assert(net_tx_fence_status(1, 1, 12, 11, false, true) == NetDrainStatus::Complete);
  assert(net_tx_fence_status(2, 1, 0, 11, false, true) == NetDrainStatus::Failed);
  assert(net_tx_fence_status(1, 1, 11, 11, true, true) == NetDrainStatus::Failed);
  assert(net_tx_fence_status(1, 1, 11, 11, false, false) == NetDrainStatus::Failed);
  // Hotspot upload still advancing after the old five-second deadline.
  NetTxDrainDeadline slow(100, 0);
  for (uint32_t elapsed = 5000; elapsed < 60000; elapsed += 5000) {
    assert(!slow.expired(100 + elapsed, elapsed / 5000, 15000, 60000));
  }
  assert(slow.expired(60100, 12, 15000, 60000)); // hard cap despite progress
  NetTxDrainDeadline stalled(100, 7);
  assert(!stalled.expired(15099, 7, 15000, 60000));
  assert(stalled.expired(15100, 7, 15000, 60000));
  NetTxDrainDeadline resumed(100, 7);
  assert(!resumed.expired(14100, 8, 15000, 60000));
  assert(!resumed.expired(28100, 9, 15000, 60000));
  assert(resumed.expired(43100, 9, 15000, 60000));
  NetTxDrainDeadline rollover(UINT32_MAX - 999, 0);
  assert(!rollover.expired(13999, 0, 15000, 60000));
  assert(rollover.expired(14000, 0, 15000, 60000));

  NetTxEpoch epoch;
  const auto first = epoch.generation();
  epoch.fail(first, "first failure");
  assert(epoch.failed());
  epoch.fail(first, "later failure");
  assert(strcmp(epoch.reason(), "first failure") == 0);
  epoch.reset(); // DISCONNECTED callback clears the queue inside sendBIN.
  epoch.fail(first, "sendBIN returned false after disconnect");
  assert(!epoch.failed());
  assert(epoch.reason()[0] == '\0');
  for (unsigned turn = 0; turn < 10000; ++turn) {
    auto in_flight = epoch.generation();
    epoch.reset(); // cancel/barge-in before the worker acquires the WS mutex.
    assert(in_flight != epoch.generation());
    epoch.fail(in_flight, "stale lock/send timeout");
    assert(!epoch.failed());
    epoch.fail(epoch.generation(), "current failure");
    assert(epoch.failed());
    epoch.reset();
  }

  Frame storage[160];
  NetTxQueue<Frame, 160, 8> queue;
  assert(queue.reserve(true) == nullptr);
  queue.attach(storage);
  // Full audio queue must still accept audio_end/cancel; neither admission
  // failure may overwrite the accepted frames or increment the queue size.
  for (unsigned i = 0; i < 152; ++i) {
    auto *slot = queue.reserve(false);
    assert(slot); *slot = {i, false}; queue.commit();
  }
  assert(queue.reserve(false) == nullptr);
  for (unsigned i = 152; i < 160; ++i) {
    auto *slot = queue.reserve(true);
    assert(slot); *slot = {i, true}; queue.commit();
  }
  assert(queue.reserve(true) == nullptr && queue.size() == 160);
  Frame actual;
  for (unsigned i = 0; i < 160; ++i) {
    assert(queue.pop(actual));
    assert(actual.sequence == i && actual.control == (i >= 152));
  }
  assert(!queue.pop(actual));

  // Exercise wraparound, repeated conversation resets and all-control floods
  // against an independent FIFO model, with nonzero head/tail on most clears.
  std::deque<Frame> model;
  uint32_t seed = 42;
  for (unsigned i = 0; i < 200000; ++i) {
    seed = seed * 1664525U + 1013904223U;
    if ((seed >> 24) == 0) {
      queue.clear(); model.clear();
    } else if ((seed & 3) == 0) {
      assert(queue.pop(actual) == !model.empty());
      if (!model.empty()) {
        assert(actual.sequence == model.front().sequence);
        assert(actual.control == model.front().control);
        model.pop_front();
      }
    } else {
      bool control = (seed & 8) != 0;
      auto *slot = queue.reserve(control);
      assert((slot != nullptr) == (model.size() < (control ? 160 : 152)));
      if (slot) {
        *slot = {i, control}; queue.commit(); model.push_back({i, control});
      }
    }
    assert(queue.size() == model.size());
  }
}
