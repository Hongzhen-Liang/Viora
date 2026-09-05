#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <deque>
#include "../src/net_tx_queue.h"

struct Frame { unsigned sequence; bool control; };
int main() {
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
