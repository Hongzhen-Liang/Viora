# micro-wake-word 官方框架训练 Hi Vesper 唤醒词

用 [micro-wake-word](https://github.com/OHF-Voice/micro-wake-word) 官方训练框架
训练自定义唤醒词 "Hi Vesper"（Piper 合成样本），产出可换回固件的流式量化
`.tflite`。

## 环境

- conda 环境：`mww`（Python 3.11），入口 `conda run -n mww python ...`
- 依赖（已装）：`microwakeword`（editable，`micro-wake-word/`）、tensorflow 2.21、
  torch 2.13（MPS）、`piper-phonemize-cross`、`pymicro_features`（puddly fork）、
  tensorboard、torchcodec

## 已完成的本地适配（对官方框架的小补丁）

1. `micro-wake-word/microwakeword/audio/clips.py`：用 `soundfile` 直接加载 WAV，
   绕开 HuggingFace `datasets`（新版需要 torchcodec/ffmpeg）。
2. `micro-wake-word/microwakeword/audio/audio_utils.py`：`pymicro_features` fork
   （macOS 编译）方法名是 `ProcessSamples`。
3. `piper-sample-generator/generate_samples.py`：`torch.load(..., weights_only=False)`
   兼容 torch 2.13。

## 数据

```
data/
  generated_samples/            # 1000 条 piper "hi vesper" 样本（0.7-0.9s）
  generated_augmented_features/ # 正样本增强特征（train 16000 / val 1000 / test 100）
  negative_tts_samples/         # 586 条英文短语：piper 800 + edge-tts 300（含大量 /aɪ/ 近音）
  negative_tts_merged/          # 合并后的负样本 wav
  generated_negative_features/  # 负样本特征（train 880 / val 110 / test 109）
  negative_datasets/            # HuggingFace kahrendt/microwakeword 预生成负样本
      speech/ dinner_party/ no_speech/ dinner_party_eval/
```

## 一键流程

```bash
cd wake_word_training/mww
conda run -n mww python scripts/generate_positive_features.py    # 正样本特征（已跑过）
conda run -n mww python scripts/generate_negative_samples.py     # 生成 TTS 负样本
conda run -n mww python scripts/generate_negative_features.py    # 负样本特征
scripts/train.sh                                                # 训练 + 转量化流式模型
```

产物：`trained_models/wakeword/tflite_stream_state_internal_quant/stream_state_internal_quant.tflite`

## 训练要点（v2 经验）

- 第一版（仅官方 HF 负样本）在 edge-tts 未知音色上对 "hey there how is it going"、
  "please turn on the lights" 等 /aɪ/ 短语误报（avg5 0.92~1.0）。
- v2 加了 **586 条英文短语的 TTS 负样本（piper + edge-tts 双引擎）** + 轻微 SpecAugment
  （time mask 50×2 / freq mask 5×2）+ 负类权重 20→40，重训后：
  - 正向 4 音色 + 2 全新音色全部 avg5 = 1.000；
  - 原误报短语降到 0.206 / 0.000，5 条全新短语 max 0.010；
  - 框架 ROC：cutoff 0.09 时 frr=0、faph=0。
- 固件 cutoff 取 0.5（正 1.0 / 负 ≤0.21，裕量充足）。

## 换回固件

```bash
cp trained_models/wakeword/tflite_stream_state_internal_quant/stream_state_internal_quant.tflite trained_models/hi_vesper.tflite
# 改 scripts/hi_vesper_manifest.json 里的 probability_cutoff（当前 0.5）
python3 ../../scripts/convert_mww_model.py trained_models/hi_vesper.tflite --json scripts/hi_vesper_manifest.json
```

然后确认 `src/config.h` 的 `WAKE_WORD` 为 "Hi Vesper" 并重新编译固件。
固件 `wake_word.cpp` 无需改动——训练模型与预训练模型契约一致
（输入 `[1,3,40]` int8，输出 `[1,1]` uint8，13 种算子）。
