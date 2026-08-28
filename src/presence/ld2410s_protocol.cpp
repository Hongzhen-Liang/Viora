#include "presence/ld2410s_protocol.h"

#include <string.h>

namespace {

constexpr uint8_t kStandardHeader[] = {0xF4, 0xF3, 0xF2, 0xF1};
constexpr uint8_t kStandardTail[] = {0xF8, 0xF7, 0xF6, 0xF5};

}  // namespace

bool parseLd2410sReport(const uint8_t *frame, size_t len,
                        Ld2410sReport *report) {
  if (frame == nullptr || report == nullptr) return false;

  // 出厂默认的极简数据格式。
  if (len == 5 && frame[0] == 0x6E && frame[4] == 0x62) {
    if (frame[1] > 3) return false;
    report->target_state = frame[1];
    report->present = frame[1] >= 2;
    report->distance_cm =
        static_cast<uint16_t>(frame[2]) |
        (static_cast<uint16_t>(frame[3]) << 8);
    report->format = Ld2410sReportFormat::kCompact;
    return true;
  }

  // 标准数据格式。payload 至少包含 type、state 和 2 字节距离。
  if (len < 14 || memcmp(frame, kStandardHeader, sizeof(kStandardHeader)) != 0 ||
      memcmp(frame + len - sizeof(kStandardTail), kStandardTail,
             sizeof(kStandardTail)) != 0) {
    return false;
  }
  const uint16_t payload_len =
      static_cast<uint16_t>(frame[4]) |
      (static_cast<uint16_t>(frame[5]) << 8);
  if (len != 4U + 2U + payload_len + 4U || payload_len < 4 ||
      frame[6] != 0x01 || frame[7] > 3) {
    return false;
  }

  report->target_state = frame[7];
  report->present = frame[7] >= 2;
  report->distance_cm =
      static_cast<uint16_t>(frame[8]) |
      (static_cast<uint16_t>(frame[9]) << 8);
  report->format = Ld2410sReportFormat::kStandard;
  return true;
}
