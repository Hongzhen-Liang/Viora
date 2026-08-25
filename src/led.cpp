// ============================================================
// 状态灯实现：用 Arduino 核心内置的 neopixelWrite（RMT 驱动，
// 无需外部库）控制板载 WS2812 RGB。
// ============================================================
#include <Arduino.h>

#include "config.h"
#include "led.h"

static LedMode  s_mode    = LED_MODE_IDLE;
static uint32_t s_last_ms = 0;

// 三角波呼吸 0..255
static uint8_t breath(uint32_t period_ms) {
  uint32_t t = millis() % period_ms;
  uint32_t half = period_ms / 2;
  return (uint8_t)((t < half ? t : period_ms - t) * 255 / half);
}

void led_init() {
#if HAS_STATUS_LED
  neopixelWrite((uint8_t)LED_PIN, 0, 0, 0);
  Serial.printf("[LED] WS2812 状态灯 @ GPIO%d\n", LED_PIN);
#else
  Serial.println("[LED] 此板无可编程状态灯，状态灯功能已禁用");
#endif
}

void led_set_mode(LedMode mode) {
  if (mode != s_mode) {
    s_mode = mode;
    // 切换时先灭灯一次，避免上一状态颜色残留
#if HAS_STATUS_LED
    neopixelWrite((uint8_t)LED_PIN, 0, 0, 0);
#endif
  }
}

void led_loop() {
#if !HAS_STATUS_LED
  return;
#else
  uint32_t now = millis();
  if (now - s_last_ms < 30) return;   // 约 30ms 刷新一帧，RMT 开销可忽略
  s_last_ms = now;

  uint8_t r = 0, g = 0, b = 0;
  switch (s_mode) {
    case LED_MODE_ERROR:              // 红色快闪（250ms 周期）
      if ((now / 250) % 2) r = 64;
      break;

    case LED_MODE_IDLE:               // 蓝色呼吸（3s 周期，亮度压到 0..63）
      b = breath(3000) / 4;
      break;

    case LED_MODE_LISTENING:          // 绿色常亮
      g = 64;
      break;

    case LED_MODE_PROCESSING:         // 琥珀色呼吸（1.2s 周期）
      {
        uint8_t v = breath(1200) / 4;
        r = v;
        g = (uint8_t)(v * 3 / 5);
      }
      break;

    case LED_MODE_PLAYING:            // 青色常亮
      g = 48;
      b = 48;
      break;

    case LED_MODE_PROVISIONING:       // 白色脉冲（1.2s 周期）——配网模式
      {
        uint8_t v = breath(1200) / 6;
        r = v;
        g = v;
        b = v;
      }
      break;
  }
  neopixelWrite((uint8_t)LED_PIN, r, g, b);
#endif
}
