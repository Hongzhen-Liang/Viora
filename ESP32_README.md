# Viora ESP32 客户端接入文档

> 本文档面向 **ESP32 端**项目，说明如何接入 Mac 上的 Viora 服务器。
>
> ESP32 端职责（只做这四件事）：
> 1. **自研唤醒词检测**（"Hi Vesper"，Log-Mel + DS-CNN INT8）
> 2. **录音上传**（16k/16bit PCM，经 WebSocket）
> 3. **接收音频播放**（服务器下发的合成语音）
> 4. **传感器采集上报**（土壤湿度 / 温湿度 / 光照）
>
> 所有语音理解、对话、合成都在 Mac 服务器端完成，ESP32 不跑 ASR/LLM/TTS。

---

## 1. 整体链路

```mermaid
sequenceDiagram
    participant E as ESP32
    participant S as Mac 服务器(8765)

    Note over E: 唤醒词 "Hi Vesper" 命中
    E->>S: {"type":"audio_start"}
    E->>S: 二进制 PCM（连续流式发送）
    E->>S: {"type":"audio_end"}
    Note over S: ASR → LLM → TTS（服务器串行执行）
    S-->>E: {"type":"tts_start","bytes":N}
    S-->>E: 二进制 PCM（可拆多块）
    S-->>E: {"type":"tts_end"}
    E->>E: I2S 播放

    Note over E,S: 旁路：每 10~30 秒
    E->>S: {"type":"telemetry","soil_moisture":23,"temp":26.5,"humidity":60,"light":4200}
```

---

## 2. 硬件选型建议

| 模块 | 推荐型号 | 说明 |
|------|----------|------|
| 主控 | ESP32-S3-WROOM-1（N16R8） | 带向量指令，跑唤醒词更省力；I2S 外设全 |
| 麦克风 | INMP441 | I2S MEMS 数字麦克风，输出 24bit（取高 16bit） |
| 功放+扬声器 | MAX98357A + 3W 小喇叭 | I2S DAC 功放，单声道 |
| 土壤湿度 | 电容式土壤湿度传感器 | 比电阻式耐腐蚀 |
| 温湿度 | DHT22 / SHT30 | I2C 或单总线 |
| 光照 | BH1750 或光敏电阻 | I2C / ADC |

### 实际引脚连接（与 `src/config.h` 一致）

| 外设 | 引脚 | 说明 |
|------|------|------|
| 麦克风 SD（数据） | GPIO 20 | MSM3526 / INMP441 I2S 数据 |
| 麦克风 SCK（位时钟） | GPIO 15 | I2S BCLK |
| 麦克风 WS（左右时钟） | GPIO 16 | I2S LRCK |
| 麦克风 VDD | GPIO 19 | 软件输出 3.3V 供电（约 1.4mA） |
| 麦克风 L/R | GPIO 17 | 输出低电平 = 左声道 |
| MAX98357 VIN | 5V | 功放供电（2.5~5.5V，5V 更响） |
| MAX98357 BCLK | GPIO 4 | I2S 位时钟 |
| MAX98357 LRC | GPIO 5 | I2S 左右时钟（WS） |
| MAX98357 DIN | GPIO 6 | I2S 音频数据 |
| MAX98357 GAIN | 悬空 | 多数模块默认 9dB，焊跳线可调 3~15dB |
| 板载 WS2812 状态灯 | GPIO 48 | 若板子在 GPIO38 改 `config.h` |
| DHT22 / BH1750 / 土壤湿度 | 待定 | `sensors.h` 为占位，传感器到货后实现 |

> 引脚可按实际板子调整，只需在 `config.h` 里改。

---

## 3. 通信协议（重要，必须严格遵守）

- 端点：`ws://<Mac IP>:8765/ws`
- 音频规格：**16kHz / 16bit / 单声道 / little-endian PCM**，每样本 2 字节。
- 帧分两类：**文本帧 = JSON 控制消息**，**二进制帧 = 音频数据**。

### 3.1 ESP32 → 服务器

| 帧类型 | 内容 | 说明 |
|--------|------|------|
| 文本 | `{"type":"audio_start","source":"wake","new_conversation":true}` | 开始上传；唤醒轮会重置旧上下文，追问轮用 `follow_up` |
| 二进制 | PCM 字节 | 音频数据，可拆成多块连续发送 |
| 文本 | `{"type":"audio_end","trim_start_ms":300,"trim_end_ms":650}` | 动态断句完成；首尾裁剪提示可减少 ASR 延迟 |
| 文本 | `{"type":"cancel","reason":"barge_in"}` | 用户在回复播放中打断，立即取消当前回复 |
| 文本 | `{"type":"telemetry","soil_moisture":23,"temp":26.5,"humidity":60,"light":4200}` | 传感器快照 |

### 3.2 服务器 → ESP32

| 帧类型 | 内容 | 说明 |
|--------|------|------|
| 文本 | `{"type":"text","user":"...","reply":"...","op":"none"}` | 回复文字；`op` 为 LLM 识别操作：`none`/`exit`/`volume_up`/`volume_down` |
| 文本 | `{"type":"tts_start","bytes":N}` | 开始下发合成音频，N 为 PCM 总字节数 |
| 二进制 | PCM 字节 | 合成音频，可拆成多块 |
| 文本 | `{"type":"tts_end"}` | 音频结束，完成播放 |
| 文本 | `{"type":"cmd","action":"pump","seconds":3}` | 控制指令（可选，二期） |
| 文本 | `{"type":"error","message":"..."}` | 出错提示 |

### 3.3 状态机约定

- 服务器收到 `audio_start` 后进入"收音频"状态，把后续二进制帧拼进缓冲区，直到 `audio_end`。
- 收到 `audio_end` 后服务器串行执行 ASR → LLM → TTS，**期间忙**：此时再发 `audio_start` 会收到 `{"type":"error","message":"服务器忙..."}`。
- 流水线完成后服务器先发 `tts_start`，再流式发二进制音频，最后 `tts_end`。
- text 帧带 `op` 字段，端侧按 op 分发：`exit`→道别音频播完回待唤醒；`volume_up`/`volume_down`→调播放音量；其余忽略。

**ESP32 端对应状态机：**

```mermaid
stateDiagram-v2
    [*] --> 待机: 上电/唤醒词未命中
    待机 --> 录音中: 唤醒词命中 → 发 audio_start
    录音中 --> 上传中: 说话结束 → 发 audio_end
    上传中 --> 播放中: 收到 tts_start
    播放中 --> 录音中: 播完后自动续聊
    播放中 --> 待机: op=exit（道别播完后回待机）
    播放中 --> 录音中: AEC 检测到用户打断 → 发 cancel
    录音中 --> 待机: 15 秒无人说话
```

### 3.4 自然轮次策略

- 唤醒与打断都保留约 900ms 前置音频，避免 KWS/VAD 的固有延迟截掉首字。
- 连续约 96ms 人声才确认开口；短回答最多等 1.2s，正常句约 0.85s，长句约 0.75s 静音即回复。
- 用户在同一句中停顿后继续说时，设备学习本轮节奏，自动把后续等待放宽，最大 1.8s。
- 回复播放完立即回到聆听；15s 没有新话题才结束会话。播放中经 AEC 后连续约 160ms 检出近端人声即可打断。
- TTS 采用流式传输，设备预缓冲约 128ms 后开播，兼顾首响速度与网络抖动下的连续性。

**板载状态灯（WS2812 RGB，GPIO48，`led.*` 已实现）**：

| 灯色 | 状态 |
|------|------|
| 红闪 | 未连接服务器 |
| 蓝呼吸 | 待唤醒（IDLE） |
| 绿常亮 | 聆听中 |
| 琥珀呼吸 | 等服务器处理 |
| 青常亮 | 播放回复 |

---

## 4. 开发环境

推荐 **PlatformIO**（VSCode 插件），依赖管理比 Arduino IDE 方便。

### `platformio.ini` 示例

```ini
[env:esp32s3]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200

lib_deps =
    links2004/WebSockets@^2.4.1
    adafruit/DHT sensor library@^1.4.6
    adafruit/Adafruit Unified Sensor@^1.1.14
    ; 音频 I2S 可选用 schreibfaul1/ESP32-audioI2S，或用 arduino-esp32 自带 I2S 驱动
```

### 关键库

| 库 | 用途 |
|----|------|
| `links2004/WebSockets` | WebSocket 客户端 |
| `arduino-esp32` 自带 I2S | 录音 / 播放（`driver/i2s.h`） |
| `schreibfaul1/ESP32-audioI2S` | （可选）封装好的 I2S 输入输出 |
| `DHT sensor library` | 温湿度 |
| ESP-SR（ESP-IDF 组件） | 神经 VAD + 降噪（AFE，见 §7） |
| TFLite Micro + ESP-NN | 自研 Hi Vesper 全 INT8 推理 |

---

## 5. 目录结构建议

```
viora-esp32/
├── platformio.ini
├── README.md              # 本文件
├── include/
│   └── config.h           # WiFi / 服务器地址 / 引脚
└── src/
    ├── main.cpp           # 主流程 + 状态机
    ├── ws_client.cpp      # WebSocket 收发
    ├── mic.cpp            # I2S 录音
    ├── speaker.cpp        # I2S 播放
    ├── wake_word.cpp      # 唤醒词检测
    └── sensors.cpp        # 传感器采集
```

### 敏感配置（`src/secrets.h`）

WiFi、服务器域名/端口、API Key 属于敏感信息，不会提交到 GitHub：

```bash
cp src/secrets.example.h src/secrets.h   # 复制模板后填写
```

`secrets.h` 内容（已被 `.gitignore` 忽略）：

```cpp
#define SECRET_WIFI_SSID    "你的WiFi名"
#define SECRET_WIFI_PASS    "你的WiFi密码"
#define SECRET_SERVER_HOST  "your-server-host"   // 域名需局域网 DNS 能解析，或直接填 IP
#define SECRET_SERVER_PORT  8765
#define SECRET_API_KEY      "与 VioraServer/.env 的 API_KEY 一致"
```

固件握手时携带 `X-Api-Key` 请求头，服务端校验失败会拒绝连接（close 1008）。
服务端 `.env` 里 `API_KEY` 留空则鉴权关闭（开发模式）。其余公开参数（引脚、
VAD、唤醒词等）仍在 `src/config.h`。
#define AUDIO_BITS         I2S_DATA_BIT_WIDTH_16BIT
#define AUDIO_CHANNELS     I2S_CHANNEL_FMT_ONLY_LEFT

// I2S 录音（INMP441）
#define I2S_MIC_BCLK   4
#define I2S_MIC_WS     5
#define I2S_MIC_DIN    6

// I2S 播放（MAX98357A）
#define I2S_SPK_BCLK  15
#define I2S_SPK_LRC   16
#define I2S_SPK_DOUT  17

// 传感器
#define DHT_PIN        8
#define SOIL_ADC_PIN   2

// 遥测上报周期（毫秒）
#define TELEMETRY_INTERVAL_MS 15000
```

---

## 6. 核心接入代码示例

下面是接入服务器的**最小可用示例**，录音/播放/唤醒词用接口函数占位，按你的硬件实现即可。

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "config.h"

// ---------- 音频接口（按你的硬件实现） ----------
bool mic_begin();                    // 初始化 I2S 麦克风
bool speaker_begin();                // 初始化 I2S 功放
int  mic_read(int16_t* buf, int samples);   // 读一帧 PCM，返回样本数
void speaker_play(const uint8_t* data, size_t len); // 播放一段 PCM
bool wake_word_detected();           // 自研唤醒词 "Hi Vesper" 是否命中

WebSocketsClient ws;
bool collecting = false;             // 是否处于"录音上传"状态
uint32_t lastTelemetry = 0;

void sendJson(const char* type) {
  StaticJsonDocument<128> doc;
  doc["type"] = type;
  char buf[128];
  serializeJson(doc, buf);
  ws.sendTXT(buf);
}

void sendTelemetry() {
  StaticJsonDocument<256> doc;
  doc["type"] = "telemetry";
  doc["soil_moisture"] = 23;   // TODO: 换成真实传感器读数
  doc["temp"] = 26.5;
  doc["humidity"] = 60;
  doc["light"] = 4200;
  char buf[256];
  serializeJson(doc, buf);
  ws.sendTXT(buf);
}

// WebSocket 事件回调
void onWsEvent(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("[ws] 已连接 %s:%d\n", SERVER_IP, SERVER_PORT);
      break;

    case WStype_TEXT: {
      StaticJsonDocument<256> doc;
      DeserializationError err = deserializeJson(doc, payload, len);
      if (err) return;
      const char* t = doc["type"];
      if (strcmp(t, "tts_start") == 0) {
        // 准备播放：可初始化播放缓冲，bytes = doc["bytes"]
        Serial.printf("[ws] 开始播放，共 %d 字节\n", (int)doc["bytes"]);
      } else if (strcmp(t, "tts_end") == 0) {
        Serial.println("[ws] 播放结束");
      } else if (strcmp(t, "error") == 0) {
        Serial.printf("[ws] 服务器错误: %s\n", (const char*)doc["message"]);
        collecting = false;   // 出错则复位状态
      }
      break;
    }

    case WStype_BIN:
      // 服务器下发的合成语音 PCM → 直接送去播放
      speaker_play(payload, len);
      break;

    case WStype_DISCONNECTED:
      Serial.println("[ws] 断开，稍后重连");
      break;
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi 已连接");

  mic_begin();
  speaker_begin();

  ws.begin(SERVER_IP, SERVER_PORT, WS_PATH);
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(3000);
}

void loop() {
  ws.loop();

  if (!ws.isConnected()) return;

  // 唤醒词命中 → 开始录音上传
  if (!collecting && wake_word_detected()) {
    collecting = true;
    sendJson("audio_start");
    Serial.println("[app] 开始录音上传");
  }

  // 录音中 → 分块上传 PCM
  if (collecting) {
    const int FRAME_SAMPLES = 1600;              // 每块 100ms
    int16_t buf[FRAME_SAMPLES];
    int n = mic_read(buf, FRAME_SAMPLES);
    if (n > 0) {
      ws.sendBIN((uint8_t*)buf, n * 2);          // 16bit → 2 字节/样本
    }
    // TODO: 用 VAD/静音检测判断说完话，这里以固定时长为例
    static uint32_t recStart = millis();
    if (millis() - recStart > 5000) {            // 最长录 5 秒
      collecting = false;
      sendJson("audio_end");
      Serial.println("[app] 录音结束，等待服务器回复");
    }
  }

  // 定时上报遥测
  if (millis() - lastTelemetry > TELEMETRY_INTERVAL_MS) {
    lastTelemetry = millis();
    sendTelemetry();
  }
}
```

> 关键点：`mic_read` 必须输出 **16k/16bit/单声道/小端** 的 PCM，`ws.sendBIN` 直接把 `int16_t` 数组按字节发送即可（小端平台无需转换）。

---

## 7. 唤醒词检测 + 断句 VAD

项目把唤醒与对话音频前端分开处理：

- **唤醒词**：原始 16 kHz PCM 经 ESP-DSP 计算 `148×40` Log-Mel，由自研轻量 DS-CNN
  full-int8 模型通过 TFLite Micro + ESP-NN 推理；最近 4 个 100 ms 滑窗中至少 3 个
  `p>=0.675`，且窗口峰值 `p>=0.85` 才触发，之后冷却 2.5 秒。这一时间证据规则用于
  缓解音乐把相邻窗口分数压低的问题，同时避免单个异常高分直接误唤醒。每次启动先运行
  Python/ESP32 golden-vector 自检。
- **断句端点**：AFE 内置**神经 VAD**（`vad_state`）判断“是否有人说话”，替代能量门限——背景音乐不会被当成人声，音乐播放中也能正确结束对话；能量法（`vad.*`）仅留作诊断。
- **降噪**：AFE 输出增强音频（NS_MODE_SSP），上传给服务器 Whisper 的也是增强后的 PCM。
- **WakeNet 已关闭**：不加载 `wn9_nihaoxiaozhi`，不生成/烧录 `srmodels.bin`，不再需要
  `model` 分区。AFE 配置为单麦、无参考通道（`aec_init=false`），内存优先放 PSRAM。

> 注意：`src/esp_afe_sr_1mic.ref` 是新版本模板，与 1.9.2 头文件不兼容，不要编译；直接用 `esp_afe_sr_models.h` 里的 `ESP_AFE_SR_HANDLE.create_from_config()`。

> 当前 N16R8 板端实测模型 37,376 bytes，tensor arena 约 122 KB（内部 SRAM），单次推理
> 约 49.2 ms；ESP32-S3 带 PSRAM 用于音频和 AFE 缓冲。

---

## 8. 传感器上报

`telemetry` 字段与服务器提示词严格对应：

| 字段 | 含义 | 单位 |
|------|------|------|
| `soil_moisture` | 土壤湿度 | %（<30 偏干） |
| `temp` | 气温 | ℃ |
| `humidity` | 空气湿度 | % |
| `light` | 光照 | lux |

每 10~30 秒上报一次即可，服务器只保留最新快照并注入 LLM 提示词。

---

## 9. 调试清单

1. **连不上**：确认 ESP32 与 Mac 同一局域网；Mac 上服务器已启动（`curl http://<Mac IP>:8765/health` 应返回 `{"service":"Viora","status":"ok"}`）。
2. **收到 `{"type":"error","message":"服务器忙..."}`**：上一轮流水线还没结束，等收到 `tts_end` 后再发下一段语音。
3. **上传后无回复**：检查麦克风采样率是否为 16k、位深 16bit、单声道；打印 `mic_read` 读到的样本数确认非 0。
4. **播放声音小/杂音**：确认 MAX98357A 的 `GAIN` 引脚接法；INMP441 的 L/R 引脚接地表示左声道。

---

## 10. 与服务器端接口速查

| 项 | 约定 |
|----|------|
| 端点 | `ws://<Mac IP>:8765/ws` |
| 采样率 | 16000 Hz |
| 位深 | 16 bit |
| 声道 | 单声道 |
| 字节序 | little-endian |
| 上行开始/结束 | `audio_start` / `audio_end` |
| 下行开始/结束 | `tts_start` / `tts_end` |
| 传感器上报 | `telemetry` |

---

## 11. 出门在外换 WiFi（手机配网，无需电脑）

固件内置**兜底配网热点**：WiFi 连续 `PROV_TIMEOUT_MS`（90 秒）连不上时，设备自动开启
热点 **`Viora-Setup`**（密码 `viora1234`），用手机即可完成配网。

### 使用步骤（iPhone / 任何手机通用）

1. 给设备上电，等约 90 秒，直到状态灯变为**白色脉冲**（或手机 WiFi 列表里出现 `Viora-Setup`）。
2. 手机 WiFi 设置里连接 `Viora-Setup`，密码 `viora1234`。
3. 连接后一般会自动弹出配网页；没弹出就用浏览器访问 `192.168.4.1`。
4. 填写新 WiFi 的名称和密码 → **保存并重启**。
5. 设备自动重启并连接新网络；连接成功后热点自动关闭，状态灯回到蓝色呼吸。

### 机制说明

- 配网保存的网络写入 **NVS（掉电不丢）**，最多保存 `PROV_MAX_NETWORKS`（4）个，新保存的优先尝试。
- 连接网络时按 **NVS 保存列表 → `secrets.h` 默认网络** 的顺序轮询，每个网络尝试 `WIFI_ATTEMPT_MS`（8 秒）。
- 以后换地方：所有候选都失败 90 秒后自动再次进入配网模式，重复上面的步骤即可。
- 配网页里可以删除不再使用的已保存网络。

> 相关配置（热点名/密码/超时等）都在 `src/config.h` 的 `PROV_*` / `WIFI_ATTEMPT_MS` 宏里。
> 注意：服务器域名 `sinscry.synology.me:11451` 走公网时需要路由器端口转发；配好 WiFi 后若连不上服务器，先检查端口转发。
