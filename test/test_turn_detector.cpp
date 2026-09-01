#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/turn_detector.h"

static const TurnDetectorConfig kConfig = {
    32, 3, 5, 450, 15000, 15000, 640,
    1200, 650, 5000, 750, 1800, 160,
};

static TurnEvent advance(TurnDetector &turn, uint32_t *now, bool speech) {
  *now += kConfig.frame_ms;
  return turn.update(*now, speech);
}

static void establish_normal_turn(TurnDetector &turn, uint32_t *now) {
  for (int i = 0; i < 3; ++i) {
    const TurnEvent event = advance(turn, now, true);
    if (i < 2) assert(event == TURN_EVENT_NONE);
    else assert(event == TURN_EVENT_SPEECH_STARTED);
  }
  for (int i = 0; i < 22; ++i) {
    assert(advance(turn, now, true) == TURN_EVENT_NONE);
  }
}

static TurnEvent advance_until_event(TurnDetector &turn, uint32_t *now,
                                     bool speech) {
  TurnEvent event = TURN_EVENT_NONE;
  while (event == TURN_EVENT_NONE) event = advance(turn, now, speech);
  return event;
}

int main() {
  // 本地确认后的音频不再带唤醒词：协议 source 是 follow_up，但仍开启新会话。
  {
    assert(strcmp(listen_source_for(LISTEN_FROM_WAKE), "wake") == 0);
    assert(strcmp(listen_source_for(LISTEN_FROM_WAKE_ACK), "follow_up") == 0);
    assert(strcmp(listen_source_for(LISTEN_FROM_FOLLOWUP), "follow_up") == 0);
    assert(strcmp(listen_source_for(LISTEN_FROM_BARGE_IN), "barge_in") == 0);
    assert(listen_starts_new_conversation(LISTEN_FROM_WAKE));
    assert(listen_starts_new_conversation(LISTEN_FROM_WAKE_ACK));
    assert(!listen_starts_new_conversation(LISTEN_FROM_FOLLOWUP));
    assert(!listen_starts_new_conversation(LISTEN_FROM_BARGE_IN));
  }

  // 唤醒词短尾音不会被当指令；有静音分隔的命令快速通过，无分隔命令
  // 则必须一直持续到 352ms，覆盖 KWS/VAD 尾音拖尾的常见长度；调用端
  // 会继续观察到 550ms，给异步 AFE 足够产出时间。
  {
    const WakeAckGateConfig config = {96, 224, 2, 2, 5};
    WakeAckGate gate(config);
    uint32_t now = 0;
    gate.reset(now);
    for (int i = 0; i < 7; ++i) {
      now += 32;
      assert(!gate.update(now, true, false));
    }
    for (int i = 0; i < 4; ++i) {
      now += 32;
      assert(!gate.update(now, false, true));
    }

    gate.reset(0);
    assert(!gate.update(32, false, true));
    assert(!gate.update(64, false, true));
    assert(gate.tail_released());
    assert(!gate.update(96, true, false));
    assert(gate.update(128, true, false));

    gate.reset(0);
    for (now = 32; now < 352; now += 32) {
      assert(!gate.update(now, true, false));
    }
    assert(gate.update(352, true, false));

    // 高能量但 AFE 暂无结果是 unknown，不能伪造静音分隔。
    gate.reset(0);
    assert(!gate.update(32, false, false));
    assert(!gate.update(64, false, false));
    assert(!gate.tail_released());
  }

  // 普通完整句在约 650ms 静音后快速提交。
  {
    TurnDetector turn(kConfig);
    uint32_t now = 0;
    turn.reset(now);
    establish_normal_turn(turn, &now);
    assert(turn.endpoint_silence_ms() == 650);
    const TurnEvent event = advance_until_event(turn, &now, false);
    assert(event == TURN_EVENT_ENDPOINT);
    assert(turn.current_silence_ms(now) >= 650);
    assert(turn.current_silence_ms(now) < 650 + kConfig.frame_ms);
  }

  // 用户曾在句中停顿后继续说，后续端点会自适应放宽。
  {
    TurnDetector turn(kConfig);
    uint32_t now = 0;
    turn.reset(now);
    establish_normal_turn(turn, &now);
    // 480ms 小于当前端点，恢复说话后应被学习，而不是提前断句。
    for (int i = 0; i < 15; ++i) {
      assert(advance(turn, &now, false) == TURN_EVENT_NONE);
    }
    assert(advance(turn, &now, true) == TURN_EVENT_NONE);
    assert(turn.max_completed_gap_ms() >= 480);
    assert(turn.endpoint_silence_ms() > 650);
  }

  // 扬声器尾音 guard 内的能量不启动录音，无人说话最终 idle timeout。
  {
    TurnDetector turn(kConfig);
    uint32_t now = 0;
    turn.reset(now, 180);
    for (int i = 0; i < 4; ++i) {
      assert(advance(turn, &now, true) == TURN_EVENT_NONE);
    }
    TurnEvent event = TURN_EVENT_NONE;
    while (event == TURN_EVENT_NONE) event = advance(turn, &now, false);
    assert(event == TURN_EVENT_IDLE_TIMEOUT);
    assert(now >= kConfig.idle_timeout_ms);
    assert(now < kConfig.idle_timeout_ms + kConfig.frame_ms);
  }

  // 每轮可动态调整续聊等待：疑问句更耐心，陈述句更快自然收会话。
  {
    TurnDetector turn(kConfig);
    uint32_t now = 0;
    turn.reset(now);
    turn.set_idle_timeout_ms(22000);
    while (now < 21984) {
      assert(advance(turn, &now, false) == TURN_EVENT_NONE);
    }
    assert(advance(turn, &now, false) == TURN_EVENT_IDLE_TIMEOUT);

    turn.reset(0);
    turn.set_idle_timeout_ms(100);  // clamp 到 5 秒
    now = 0;
    while (now < 4992) {
      assert(advance(turn, &now, false) == TURN_EVENT_NONE);
    }
    assert(advance(turn, &now, false) == TURN_EVENT_IDLE_TIMEOUT);
  }

  // 唤醒/打断的前置证据会启动一轮，但等待现场首字不污染节奏学习。
  {
    TurnDetector turn(kConfig);
    turn.reset(1000);
    turn.prime_speech(1000, 5);
    assert(turn.speech_started());
    assert(turn.voice_frames() == 5);
    assert(turn.speech_start_ms() == 840);
    assert(turn.update(2000, false) == TURN_EVENT_NONE);
    assert(turn.update(2100, true) == TURN_EVENT_NONE);
    assert(turn.max_completed_gap_ms() == 0);
  }

  // 五帧短句（例如“等等”）不是噪声，并在较耐心的 1.2s 端点提交。
  {
    TurnDetector turn(kConfig);
    uint32_t now = 0;
    turn.reset(now);
    for (int i = 0; i < 5; ++i) {
      const TurnEvent event = advance(turn, &now, true);
      if (i == 2) assert(event == TURN_EVENT_SPEECH_STARTED);
      else assert(event == TURN_EVENT_NONE);
    }
    assert(turn.endpoint_silence_ms() == 1200);
    assert(advance_until_event(turn, &now, false) == TURN_EVENT_ENDPOINT);
  }

  // 仅够启动门限但不足最小语音长度的短促噪声会被拒绝并继续聆听。
  {
    TurnDetector turn(kConfig);
    uint32_t now = 0;
    turn.reset(now);
    assert(advance(turn, &now, true) == TURN_EVENT_NONE);
    assert(advance(turn, &now, true) == TURN_EVENT_NONE);
    assert(advance(turn, &now, true) == TURN_EVENT_SPEECH_STARTED);
    assert(advance_until_event(turn, &now, false) == TURN_EVENT_SHORT_NOISE);
    assert(!turn.speech_started());
    assert(advance(turn, &now, false) == TURN_EVENT_NONE);
  }

  // 持续人声也必须被 15s hard endpoint 截断，避免一轮无限增长。
  {
    TurnDetector turn(kConfig);
    uint32_t now = 0;
    turn.reset(now);
    assert(advance(turn, &now, true) == TURN_EVENT_NONE);
    assert(advance(turn, &now, true) == TURN_EVENT_NONE);
    assert(advance(turn, &now, true) == TURN_EVENT_SPEECH_STARTED);
    const uint32_t speech_start = turn.speech_start_ms();
    assert(advance_until_event(turn, &now, true) == TURN_EVENT_ENDPOINT);
    assert(now - speech_start >= kConfig.max_record_ms);
    assert(now - speech_start < kConfig.max_record_ms + kConfig.frame_ms);
  }

  // 新鲜 neural=silence 始终否决持续高能背景，不形成空白 ASR 轮。
  {
    SpeechEvidenceGate gate({2, 6});
    TurnDetector turn(kConfig);
    uint32_t now = 0;
    turn.reset(now);
    TurnEvent event = TURN_EVENT_NONE;
    while (event == TURN_EVENT_NONE) {
      now += kConfig.frame_ms;
      const bool speech = gate.update(true, false, true);
      assert(!speech);
      event = turn.update(now, speech);
    }
    assert(event == TURN_EVENT_IDLE_TIMEOUT);
  }

  // AFE 真正停产时，强能量需连续六帧才能召回；新的 neural 结果立即接管。
  {
    SpeechEvidenceGate gate({2, 6});
    for (int i = 0; i < 5; ++i) assert(!gate.update(false, false, true));
    assert(gate.update(false, false, true));
    assert(!gate.update(true, false, true));
    assert(gate.update(true, true, false));
    assert(gate.update(false, false, false));
    assert(gate.update(false, false, false));
    assert(!gate.update(false, false, false));
  }

  // PROCESSING/PLAYING watchdog 共用的计时需在 millis() 回绕时仍正确。
  {
    assert(!elapsed_at_least(44999, 0, 45000));
    assert(elapsed_at_least(45000, 0, 45000));
    const uint32_t since = UINT32_MAX - 99;
    assert(!elapsed_at_least(49, since, 150));
    assert(elapsed_at_least(50, since, 150));
  }

  return 0;
}
