# Hi Vesper 自研唤醒词：现有数据与个人录音

Viora 的目标唤醒短语固定为 **Hi Vesper**（`high VESS-per`）。模型、Log-Mel、
DS-CNN、INT8 量化与 ESP32-S3 触发逻辑由本项目实现，不使用 Espressif WakeNet 模型。

当前版本复用仓库已有的 `data/`，并加入 6 条个人 `Hi Vesper` 录音。训练、全 INT8
转换、ESP32-S3 前端、TFLite Micro 推理和烧录链路均已完成，设备端不再加载
“你好小智”WakeNet。

## 现有数据

导入器实际识别到 3480 个 WAV：

| 新标签 | 来源 | 数量 | 分组依据 |
|---|---|---:|---|
| `wake` | `data/wake_word/tts` | 365 | Edge TTS voice |
| `wake` | `data/validation` | 40 | Edge TTS voice；不沿用旧 split |
| `wake` | `data/wake_word/human` | 6 | 同一真人 speaker；固定只进入 train |
| `unknown` | Speech Commands 真人语音 | 2500 | 文件名中的 speaker hash |
| `unknown` | 普通英文句子 TTS | 295 | Edge TTS voice |
| `noise` | Speech Commands environment | 270 | 原始 background source |
| `noise` | `data/background` | 4 | noise source |
| `hard_negative` | 无 | 0 | — |

所有文件都是 16 kHz、mono、PCM16。原始时长约 0.427–3.312 秒；训练前端会在 Log-Mel
之前统一做 1.5 秒的裁剪/补零。导入器不会把 ordinary unknown 错标成 hard negative。

现有数据可以训练一个 baseline，但存在两个明确限制：

- 真人 wake 目前只有同一 speaker、同一批次的 6 条，设备/房间/距离覆盖仍然很有限；
- 没有 `Hi Jasper`、`Hi Casper`、`Hey Vesper`、`Hello Vesper` 等近音 hard negative。

因此 baseline 指标只能用于验证训练和部署链路，不能代表真实环境唤醒率或近音词误触率。

## 固定模型输入契约

| 项目 | 固定值 |
|---|---:|
| model audio window | 16000 Hz / mono / 1.5 s / 24000 samples |
| frame / hop | 480 / 160 samples（30 ms / 10 ms） |
| FFT | 512 points |
| Mel | 40 bins，80–7600 Hz |
| 分类 | `wake=0`, `unknown=1`, `noise=2` |
| hard negative | 原始来源单独保留，训练时映射为 `unknown` |

常量集中在 `scripts/hi_vesper_config.py`。后续 Python、TFLite 与 ESP32 frontend 必须使用
同一数学定义。

## 项目结构

```text
wake_word_training/
├── data/                              # 唯一的 legacy 音频源，原样保留
├── run.sh                             # 个人录音 → 最终 INT8/固件资产的一键流水线
├── dataset/
│   ├── raw/{wake,hard_negative,unknown,noise}/
│   ├── train/{wake,unknown,noise}/
│   ├── val/{wake,unknown,noise}/
│   └── test/{wake,unknown,noise}/
├── scripts/
│   ├── hi_vesper_config.py
│   ├── import_human_wake.py           # M4A/WAV → 16kHz mono 1.5s PCM16
│   ├── import_legacy_data.py          # data/ → dataset/raw 相对符号链接
│   ├── split_dataset.py               # speaker/source-safe split
│   ├── train.py / modeling.py          # 轻量 DS-CNN 训练
│   ├── convert_int8.py / evaluate.py   # 全 INT8 转换与独立测试
│   └── export_firmware_assets.py       # 模型/Mel/golden vector → C++
├── tests/
└── models/                            # 当前训练模型、INT8 模型与评估元数据
```

`dataset/raw` 和 train/val/test 默认都是相对符号链接，不会再复制一份 132 MB 音频。
`data/` 仍是唯一真实文件来源。

## 1. 环境

```bash
cd wake_word_training
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

当前流水线只读取已有录音，不需要 `sounddevice`、PortAudio 或麦克风权限。导入 M4A
需要 `ffmpeg`；macOS 可执行 `brew install ffmpeg`。

`run.sh` 默认复用 `VioraServer/.venv/bin/python`（服务端环境里已装好依赖）；若想用
上面的独立 `.venv`，把 `HI_VESPER_PYTHON` 指过去即可。

## 2. 一键加入个人录音并训练

当前 6 个 Downloads 录音已配置为默认输入，直接执行：

```bash
cd wake_word_training
./run.sh
```

也可以传入任意数量的 M4A/WAV：

```bash
./run.sh /path/to/hi-vesper-01.m4a /path/to/hi-vesper-02.wav
```

脚本会依次完成：格式转换与静音裁剪、legacy 映射、无 speaker 泄漏拆分、测试、训练、
full-INT8 转换、独立测试集评估，以及 ESP32 C++ 资产导出。个人录音固定进入 train，
默认按 8 倍权重参与数据增强；可用 `HI_VESPER_HUMAN_REPEAT` 调整。

## 3. 手动建立 legacy 数据映射

先扫描全部文件，不写入：

```bash
python scripts/import_legacy_data.py --dry-run
```

预期关键输出：

```text
files=3480
wake=411 hard_negative=0 unknown=2795 noise=274
```

确认后创建相对符号链接和 `dataset/legacy_import_manifest.csv`：

```bash
python scripts/import_legacy_data.py
```

脚本可重复运行；指向相同源文件的链接会显示为 `reused`。如果目标处已有不同文件，脚本
会停止而不会覆盖。若环境不支持符号链接，可显式使用 `--mode copy`。

映射规则保证：

- 同一 Edge TTS voice 的 wake 与 unknown 永远使用相同 group；
- 同一个 Speech Commands speaker 的多个单词不会跨 split；
- 从同一条 background 长音频切出的多个 noise clip 不会跨 split；
- 旧 `data/validation` 会重新按 voice 分配，避免同一 TTS voice 同时出现在训练和评估中。

## 4. 手动创建 train/val/test

legacy 音频尚未统一到 1.5 秒，因此建 split 时需要明确允许原始时长；训练前端再统一窗口：

```bash
python scripts/split_dataset.py \
  --group-by speaker \
  --allow-nonstandard-duration \
  --dry-run
```

确认 group assignment 后创建默认 70/15/15 左右的符号链接拆分：

```bash
python scripts/split_dataset.py \
  --group-by speaker \
  --allow-nonstandard-duration \
  --force-train-speaker legacy-wake-human-hongzhenliang
```

脚本会输出 `dataset/split_manifest.csv` 并检查：

- 所有音频必须是 16 kHz、mono、PCM16；
- `wake`、`unknown`、`noise` 各自至少覆盖三个独立 group；
- 一个 speaker/voice/noise source 只能属于一个 split；
- hard negative 若存在则映射为 `unknown`，但 manifest 仍保留原始来源；
- split 已有音频时停止，只有显式传 `--overwrite` 才重建；
- 默认 `--mode symlink`，可切换为 `--mode copy`。

每次运行都会明确打印 `hard_negative=0` 警告。这是数据事实，不应通过错误改标隐藏。

## 5. 手动训练、全 INT8 转换与评估

```bash
python scripts/train.py --epochs 40 --batch-size 32 --seed 42 --human-wake-repeat 8
python scripts/convert_int8.py --representative-count 300 --seed 42
python scripts/evaluate.py
python scripts/export_firmware_assets.py
```

最终 DS-CNN 使用 `16/24/32/48/64/80` 通道、14,715 个参数；模型只包含
`Conv2D`、`DepthwiseConv2D`、`Mean`、`FullyConnected` 和 `Softmax`，输入输出均为
`int8`。生成的 `models/hi_vesper_int8.tflite` 为 37,376 字节。

当前复用数据的独立 test split（520 条）结果：

| 指标 | 结果 |
|---|---:|
| INT8 accuracy | 96.15% |
| wake recall（分类） | 100%（60/60） |
| noise recall | 66.67%（30/45；其余均分成 unknown） |
| `p >= 0.97` | wake recall 100%，负样本误触发 0/460 |
| `p >= 0.90` | wake recall 100%，负样本误触发 0/460 |

表中仍保留单窗口阈值指标，便于比较模型版本；当前固件实际采用时间证据规则：最近 4 个
100 ms 滑窗中至少 3 个 `p >= 0.675`，且窗口峰值 `p >= 0.85` 才触发，之后冷却 2.5 秒。
该规则是在不重训练模型的前提下，使用现有音乐/噪声流式混合集选择的。noise/unknown
互相混淆不会直接触发 wake，但单窗口统计仍不等于流式误唤醒率；尤其现有数据没有 hard
negative，仍需实机持续观察。个人录音固定在 train，因此上述独立 test 指标不把同一个人的
录音混入测试集。

作为训练拟合诊断，6 条个人录音在最终 INT8 模型上全部达到 `p >= 0.97`，最低为
`0.9766`；该结果不能替代新环境下的独立真人测试。

## 6. ESP32-S3 构建与烧录

从仓库根目录执行：

```bash
python3 scripts/fetch_components.py
pio run -e rymcu-esp32-s3-devkitc-1
pio run -e rymcu-esp32-s3-devkitc-1 -t upload
pio device monitor -b 115200
```

`fetch_components.py` 固定下载与 Arduino/ESP-IDF 4.4 兼容的 ESP-SR 1.9.2、ESP-DSP
1.4.0、esp-tflite-micro 1.3.2 和 ESP-NN 1.1.0。ESP-SR 只保留神经 VAD/降噪；
WakeNet、`srmodels.bin` 和 `model` 分区均已移除。

每次启动都会用一条 24,000-sample golden PCM 在板上重算 Log-Mel，再同时校验量化输入和
模型输出。当前 ESP32-S3 N16R8 实测输入/输出差异均为 0，推理约 49.2 ms，tensor arena
约 122 KB（内部 SRAM）。看到下面日志才表示部署有效：

```text
[KWS] Golden test: input max=0 ... output=[0.9961 0.0039 0.0000] ... inference=49xxx us
[KWS] Golden vector 自检通过
>>> 语音识别就绪，请说唤醒词：Hi Vesper
```

## 7. 测试与验收

```bash
python -m unittest discover -s tests -v
python -m compileall -q scripts tests
```

以现有数据为前提，数据与部署验收标准是：

- 3480 个 WAV 全部被映射，无遗漏、无覆盖、无音频复制；
- `split_dataset.py --dry-run` 通过，任何 voice/speaker/noise source 不跨 split；
- train/val/test 均包含 wake、ordinary unknown 和 noise；
- manifest 保留源标签、分组、格式、时长和路径；
- 文档和输出明确标识没有 hard negative，真人 wake 数据只有单 speaker 的 6 条；
- TFLite 输入/输出是 `int8`，Python 与固件 golden vector 一致；
- 固件构建、烧录、连续启动自检通过，串口不再出现“你好小智”模型加载信息。
