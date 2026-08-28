#pragma once

#include <stddef.h>
#include <stdint.h>

enum class Ld2410sReportFormat : uint8_t {
  kUnknown = 0,
  kCompact,
  kStandard,
};

struct Ld2410sReport {
  bool present = false;
  uint8_t target_state = 0;
  uint16_t distance_cm = 0;
  Ld2410sReportFormat format = Ld2410sReportFormat::kUnknown;
};

// 解析 LD2410S 官方协议的两种工作上报：
// - 默认极简帧：6E + state + distance_le16 + 62
// - 标准帧：F4 F3 F2 F1 + len + type(01) + state + distance_le16 + ... + tail
bool parseLd2410sReport(const uint8_t *frame, size_t len,
                        Ld2410sReport *report);
