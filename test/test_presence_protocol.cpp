#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/presence/ld2410s_protocol.h"

int main() {
  // 出厂默认极简帧：有人，距离 180cm。
  {
    const uint8_t frame[] = {0x6E, 0x02, 0xB4, 0x00, 0x62};
    Ld2410sReport report;
    assert(parseLd2410sReport(frame, sizeof(frame), &report));
    assert(report.present);
    assert(report.target_state == 2);
    assert(report.distance_cm == 180);
    assert(report.format == Ld2410sReportFormat::kCompact);
  }

  // 极简帧的 0/1 都表示无人；错误尾标记必须拒绝。
  {
    uint8_t frame[] = {0x6E, 0x01, 0x00, 0x00, 0x62};
    Ld2410sReport report;
    assert(parseLd2410sReport(frame, sizeof(frame), &report));
    assert(!report.present);
    frame[4] = 0x00;
    assert(!parseLd2410sReport(frame, sizeof(frame), &report));
  }

  // 官方标准帧：type 位于 byte 6，状态位于 byte 7，距离从 byte 8 开始。
  {
    uint8_t frame[80] = {};
    const uint8_t header[] = {0xF4, 0xF3, 0xF2, 0xF1};
    const uint8_t tail[] = {0xF8, 0xF7, 0xF6, 0xF5};
    memcpy(frame, header, sizeof(header));
    frame[4] = 70;  // type + state + distance + reserved + 16 gates * 4B
    frame[5] = 0;
    frame[6] = 0x01;
    frame[7] = 0x03;
    frame[8] = 0x69;
    frame[9] = 0x00;
    memcpy(frame + sizeof(frame) - sizeof(tail), tail, sizeof(tail));

    Ld2410sReport report;
    assert(parseLd2410sReport(frame, sizeof(frame), &report));
    assert(report.present);
    assert(report.target_state == 3);
    assert(report.distance_cm == 105);
    assert(report.format == Ld2410sReportFormat::kStandard);

    frame[6] = 0x03;  // 自动门限进度帧，不是人在数据。
    assert(!parseLd2410sReport(frame, sizeof(frame), &report));
  }

  return 0;
}
