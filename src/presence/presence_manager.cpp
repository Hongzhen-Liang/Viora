#include "presence/presence_manager.h"

#include <HardwareSerial.h>
#include <string.h>

#include "config.h"
#include "hardware/hardware_config.h"

PresenceManager g_presence;

namespace {
HardwareSerial s_radar_uart(1);

constexpr uint8_t kReportHeader[] = {0xF4, 0xF3, 0xF2, 0xF1};
constexpr uint8_t kReportTail[] = {0xF8, 0xF7, 0xF6, 0xF5};

bool hasPrefix(const uint8_t *data, size_t len, const uint8_t *prefix,
               size_t prefix_len) {
  return len >= prefix_len && memcmp(data, prefix, prefix_len) == 0;
}
}  // namespace

bool PresenceManager::begin() {
  pinMode(LD2410S_OT2_PIN, INPUT);
  s_radar_uart.setRxBufferSize(256);
  s_radar_uart.begin(LD2410S_UART_BAUD, SERIAL_8N1, LD2410S_OT1_PIN,
                     LD2410S_RX_PIN);
  const uint32_t now = millis();
  data_.absent_since_ms = now;
  candidate_since_ms_ = now;
  Serial.printf(
      "[PRESENCE] LD2410S ready: OT1/TX->GPIO%d RX<-GPIO%d OT2->GPIO%d "
      "UART=%d\n",
      LD2410S_OT1_PIN, LD2410S_RX_PIN, LD2410S_OT2_PIN,
      LD2410S_UART_BAUD);
  return true;
}

void PresenceManager::queueEvent(PresenceEvent event) {
  // 进入/离开不能被距离事件覆盖；主循环每轮都会消费，正常不会积压。
  if (pending_event_ == PresenceEvent::kNone ||
      event == PresenceEvent::kEntered || event == PresenceEvent::kLeft) {
    pending_event_ = event;
  }
}

PresenceEvent PresenceManager::takeEvent() {
  const PresenceEvent event = pending_event_;
  pending_event_ = PresenceEvent::kNone;
  return event;
}

void PresenceManager::processFrame(const uint8_t *frame, size_t len) {
  if (len < 13 || !hasPrefix(frame, len, kReportHeader, sizeof(kReportHeader)) ||
      memcmp(frame + len - sizeof(kReportTail), kReportTail,
             sizeof(kReportTail)) != 0) {
    return;
  }
  const uint16_t payload_len =
      static_cast<uint16_t>(frame[4]) |
      (static_cast<uint16_t>(frame[5]) << 8);
  if (len != 4U + 2U + payload_len + 4U || payload_len < 3) return;

  const uint8_t target_state = frame[6];
  if (target_state > 3) return;
  const bool present = target_state >= 2;
  const uint16_t distance_cm =
      static_cast<uint16_t>(frame[7]) |
      (static_cast<uint16_t>(frame[8]) << 8);
  last_uart_ms_ = millis();
  data_.uart_online = true;
  updateCandidate(present, present ? distance_cm : 0, true);
}

void PresenceManager::consumeUart() {
  while (s_radar_uart.available() > 0) {
    const uint8_t byte = static_cast<uint8_t>(s_radar_uart.read());
    if (frame_len_ < sizeof(frame_)) frame_[frame_len_++] = byte;

    // 丢弃头部噪声，但保留可能构成下一帧头的后缀。
    while (frame_len_ > 0 &&
           !hasPrefix(kReportHeader, sizeof(kReportHeader), frame_,
                      frame_len_ < sizeof(kReportHeader)
                          ? frame_len_
                          : sizeof(kReportHeader))) {
      memmove(frame_, frame_ + 1, --frame_len_);
      frame_expected_ = 0;
    }
    if (frame_len_ >= 6 && frame_expected_ == 0) {
      const uint16_t payload_len =
          static_cast<uint16_t>(frame_[4]) |
          (static_cast<uint16_t>(frame_[5]) << 8);
      frame_expected_ = 4U + 2U + payload_len + 4U;
      if (frame_expected_ < 13 || frame_expected_ > sizeof(frame_)) {
        memmove(frame_, frame_ + 1, --frame_len_);
        frame_expected_ = 0;
      }
    }
    if (frame_expected_ > 0 && frame_len_ >= frame_expected_) {
      processFrame(frame_, frame_expected_);
      const size_t remaining = frame_len_ - frame_expected_;
      if (remaining > 0) memmove(frame_, frame_ + frame_expected_, remaining);
      frame_len_ = remaining;
      frame_expected_ = 0;
    }
  }
}

void PresenceManager::commitState(bool present, uint32_t now) {
  if (data_.present == present) return;
  data_.present = present;
  if (present) {
    data_.present_since_ms = now;
    queueEvent(PresenceEvent::kEntered);
    Serial.printf("[PRESENCE] 有人进入，距离=%ucm source=%s\n",
                  data_.distance_cm, data_.uart_online ? "UART" : "OT2");
  } else {
    data_.absent_since_ms = now;
    data_.present_since_ms = 0;
    data_.distance_cm = 0;
    near_ = false;
    queueEvent(PresenceEvent::kLeft);
    Serial.println("[PRESENCE] 已确认无人");
  }
}

void PresenceManager::updateCandidate(bool present, uint16_t distance_cm,
                                      bool from_uart) {
  const uint32_t now = millis();
  data_.last_update_ms = now;
  if (from_uart && present && distance_cm > 0) data_.distance_cm = distance_cm;

  if (!candidate_initialized_ || candidate_present_ != present) {
    candidate_initialized_ = true;
    candidate_present_ = present;
    candidate_since_ms_ = now;
  }
  const uint32_t confirm_ms =
      candidate_present_ ? PRESENCE_ENTER_CONFIRM_MS : PRESENCE_LEAVE_CONFIRM_MS;
  if (now - candidate_since_ms_ >= confirm_ms) commitState(candidate_present_, now);

  if (data_.present && data_.distance_cm > 0) {
    const bool new_near = near_
                              ? data_.distance_cm <=
                                    PRESENCE_NEAR_DISTANCE_CM +
                                        PRESENCE_DISTANCE_HYSTERESIS_CM
                              : data_.distance_cm <= PRESENCE_NEAR_DISTANCE_CM;
    if (new_near && !near_) queueEvent(PresenceEvent::kApproached);
    near_ = new_near;
  }
}

void PresenceManager::poll() {
  consumeUart();
  const uint32_t now = millis();
  if (last_uart_ms_ != 0 && now - last_uart_ms_ <= PRESENCE_UART_STALE_MS) return;
  if (data_.uart_online) {
    data_.uart_online = false;
    Serial.println("[PRESENCE] UART 上报超时，回退 OT2 状态");
  }
  updateCandidate(digitalRead(LD2410S_OT2_PIN) == HIGH, 0, false);
}
