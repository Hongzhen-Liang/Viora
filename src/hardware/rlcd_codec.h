#pragma once

// 初始化 Waveshare ESP32-S3-RLCD-4.2 板载 ES7210 双麦 ADC、
// ES8311 DAC 与 NS4150B 功放。调用前必须先启动 I2S/MCLK。
bool rlcdCodecBegin();

