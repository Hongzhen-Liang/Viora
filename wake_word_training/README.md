# Viora 唤醒词训练

当前固件唤醒词 **"Hi Vesper"** 使用开源
[micro-wake-word](https://github.com/OHF-Voice/micro-wake-word) 官方框架训练，
完整流程见 **[mww/README.md](mww/README.md)**。

训练产物经 `scripts/convert_mww_model.py` 换回固件
`src/mww_model_data.*` / `src/mww_model_config.h`，并同步 `config.h` 的 `WAKE_WORD`。
