# Viora 表情素材

`1 idle_normal.png` 是造型参考图（已局部优化眼睑与瞳孔）；编号 2–13 使用内置 image_gen 分别生成，统一为单片笑嘴、黑白细线花瓣与右侧双花苞。`Overall.png` 是历史设计参考板，不参与动画。

完整生成提示词与输出来源记录在 `expression-generation.json`；第 1 张后续眼睛修正记录在 `idle-eye-refinement.json`。

## 动画

- 待机和聆听：长停顿、160 ms 短眨眼，待机偶尔侧目。
- 感知：260 ms 注意到、420 ms 确认，然后保持关注。
- 思考、说话：使用不同长度的停顿循环；说话是表情节奏，未做音素口型同步。
- 23:00–06:00 待机采用静态睡眠表情，沿用设备原有时间规则。

时序与动态区域定义在 `src/display/expression_animation.h`。显示使用第 1 张作为固定花瓣/枝条底图，叠加当前表情的面部区域；睡眠另外叠加 Z 标记。这样可以消除生成图纹理差异造成的花瓣闪动。ST7305 只发送实际变化的完整 tile 行。

## 重新生成点阵和预览

在项目根目录运行（Python 需要 Pillow）：

```sh
python3 scripts/gen_orchid_expressions.py assets src/display/orchid_expressions.h
python3 scripts/preview_orchid_expressions.py
```

所有 PNG 必须和参考图具有相同画布尺寸。打包使用同一裁剪框，避免逐帧缩放跳动；如果参考图构图变化，也需检查动画头文件中的面部和睡眠标记区域。

打开 `animation-preview.html` 可切换六种状态，查看与固件一致的点阵、区域叠加和时序，以及全部原始 PNG。浏览器预览不模拟硬件 SPI 传输延迟。

本次验证：13 张点阵尺寸与唯一性检查、动画时序边界测试、PlatformIO 固件编译、浏览器预览。未刷写设备，收音/播放期间的实际显示延迟仍需实机确认。
