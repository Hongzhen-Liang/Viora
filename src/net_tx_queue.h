#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum class NetDrainStatus { Pending, Complete, Failed };

// A fence belongs to one queue epoch. Later controls do not extend it, and
// clearing a queue must not be mistaken for successful delivery.
inline NetDrainStatus net_tx_fence_status(uint32_t epoch, uint32_t expected_epoch,
                                          uint32_t completed, uint32_t target,
                                          bool failed, bool connected) {
  if (epoch != expected_epoch || failed || !connected) return NetDrainStatus::Failed;
  return completed >= target ? NetDrainStatus::Complete : NetDrainStatus::Pending;
}

// An advancing upload may exceed the idle limit, but never the total limit.
// Unsigned subtraction also handles the millis() rollover.
class NetTxDrainDeadline {
 public:
  NetTxDrainDeadline(uint32_t now, uint32_t progress)
      : started_(now), advanced_(now), progress_(progress) {}
  bool expired(uint32_t now, uint32_t progress, uint32_t idle_ms,
               uint32_t total_ms) {
    if (progress != progress_) {
      progress_ = progress;
      advanced_ = now;
    }
    return now - started_ >= total_ms || now - advanced_ >= idle_ms;
  }
 private:
  uint32_t started_, advanced_, progress_;
};

// Caller supplies storage (PSRAM on device) and synchronization. Never moves
// queued payloads or evicts accepted control frames when the queue is full.
template <typename Frame, size_t Capacity, size_t ControlReserve>
class NetTxQueue {
  static_assert(ControlReserve < Capacity, "reserve must fit queue");
 public:
  void attach(Frame *storage) { storage_ = storage; clear(); }
  void clear() { head_ = tail_ = count_ = 0; }
  size_t size() const { return count_; }
  Frame *back() {
    return count_ ? &storage_[(head_ + Capacity - 1) % Capacity] : nullptr;
  }
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
  // Merge only adjacent frames approved by the caller. Return the number of
  // ORIGINAL queue entries consumed, so audio_end fences remain meaningful.
  template <typename Merge>
  size_t popMerged(Frame &frame, Merge merge) {
    if (!pop(frame)) return 0;
    size_t consumed = 1;
    while (count_ && merge(frame, storage_[tail_])) {
      tail_ = (tail_ + 1) % Capacity;
      --count_;
      ++consumed;
    }
    return consumed;
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
