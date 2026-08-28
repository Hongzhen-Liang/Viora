#pragma once

#include <Arduino.h>

enum class PresenceEvent : uint8_t {
  kNone,
  kEntered,
  kLeft,
  kApproached,
};

struct PresenceData {
  bool present = false;
  bool uart_online = false;
  uint16_t distance_cm = 0;
  uint32_t present_since_ms = 0;
  uint32_t absent_since_ms = 0;
  uint32_t last_update_ms = 0;
};

// LD2410S UART + OT2 状态融合。UART 数据新鲜时使用其状态和距离，
// UART 暂时不可用时自动回退到 OT2，外层逻辑无需关心通信细节。
class PresenceManager {
 public:
  bool begin();
  void poll();

  const PresenceData &data() const { return data_; }
  bool present() const { return data_.present; }
  PresenceEvent takeEvent();

 private:
  void consumeUart();
  void processFrame(const uint8_t *frame, size_t len);
  void updateCandidate(bool present, uint16_t distance_cm, bool from_uart);
  void commitState(bool present, uint32_t now);
  void queueEvent(PresenceEvent event);

  PresenceData data_;
  PresenceEvent pending_event_ = PresenceEvent::kNone;
  uint8_t frame_[96] = {};
  size_t frame_len_ = 0;
  size_t frame_expected_ = 0;
  bool candidate_present_ = false;
  bool candidate_initialized_ = false;
  uint32_t candidate_since_ms_ = 0;
  uint32_t last_uart_ms_ = 0;
  bool near_ = false;
};

extern PresenceManager g_presence;
