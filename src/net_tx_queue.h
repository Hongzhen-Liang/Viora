#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Caller supplies storage (PSRAM on device) and synchronization. Never moves
// queued payloads or evicts accepted control frames when the queue is full.
template <typename Frame, size_t Capacity, size_t ControlReserve>
class NetTxQueue {
  static_assert(ControlReserve < Capacity, "reserve must fit queue");
 public:
  void attach(Frame *storage) { storage_ = storage; clear(); }
  void clear() { head_ = tail_ = count_ = 0; }
  size_t size() const { return count_; }
  Frame *reserve(bool control) {
    if (!storage_ || count_ >= (control ? Capacity : Capacity - ControlReserve))
      return nullptr;
    return &storage_[head_];
  }
  void commit() { head_ = (head_ + 1) % Capacity; ++count_; }
  bool pop(Frame &frame) {
    if (!count_) return false;
    frame = storage_[tail_];
    tail_ = (tail_ + 1) % Capacity;
    --count_;
    return true;
  }
 private:
  Frame *storage_ = nullptr;
  size_t head_ = 0, tail_ = 0, count_ = 0;
};

// Protected by the same caller-owned lock as the queue. A completion from an
// older turn/connection must never re-latch a fault after clear/disconnect.
class NetTxEpoch {
 public:
  uint32_t generation() const { return generation_; }
  bool failed() const { return failed_; }
  const char *reason() const { return reason_; }
  void reset() {
    if (++generation_ == 0) ++generation_;
    failed_ = false;
    reason_[0] = '\0';
  }
  void fail(uint32_t generation, const char *reason) {
    if (generation != generation_ || failed_) return;
    failed_ = true;
    snprintf(reason_, sizeof(reason_), "%s", reason ? reason : "TX failure");
  }
 private:
  uint32_t generation_ = 1;
  bool failed_ = false;
  char reason_[64] = {};
};
