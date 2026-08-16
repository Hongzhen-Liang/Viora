#pragma once
// ============================================================
// 本地唤醒确认音：KWS 命中后立即播放，不等服务器（Siri 式即时响应）。
// 16kHz / 16bit / 单声道 / little-endian PCM，时长约 350 ms。
// 由 scripts/gen_wake_ack.py 用 VioraServer 当前 TTS 音色生成，
// 换音色/换文案后重跑该脚本并重新编译固件。
// ============================================================
#include <stddef.h>
#include <stdint.h>

extern const uint8_t wake_ack_pcm_data[];
extern const size_t wake_ack_pcm_len;
