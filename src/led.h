#pragma once
// ============================================================
// 状态指示模块：不带可编程灯的板卡会自动使用空实现。
// 颜色即状态，一眼可辨：
//   红闪   = 未连接服务器
//   蓝呼吸 = 待唤醒（IDLE）
//   绿常亮 = 聆听中
//   琥珀呼吸 = 等服务器处理
//   青常亮 = 播放回复
//   白脉冲 = WiFi 配网模式（连热点 Viora-Setup 配置网络）
// ============================================================
#include <Arduino.h>

enum LedMode {
  LED_MODE_ERROR = 0,   // 红色快闪
  LED_MODE_IDLE,        // 蓝色呼吸
  LED_MODE_LISTENING,   // 绿色常亮
  LED_MODE_PROCESSING,  // 琥珀色呼吸
  LED_MODE_PLAYING,     // 青色常亮
  LED_MODE_PROVISIONING,// 白色脉冲（配网中）
};

void led_init();
void led_set_mode(LedMode mode);
void led_loop();        // 主循环调用，驱动呼吸/闪烁动画
