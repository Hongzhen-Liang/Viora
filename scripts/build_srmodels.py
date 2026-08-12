#!/usr/bin/env python3
"""
生成 ESP-SR 模型镜像 srmodels.bin，供烧录到 'model' 数据分区。

选择模型：
  - 唤醒词: wn9_nihaoxiaozhi（中文"你好小智"）
  - 命令词: mn5q8_cn（中文命令词，量化版，支持"打开灯/关闭灯"等固定命令）

依赖 esp-sr 包自带的 pack_model.py 生成镜像。
"""
import math
import os
import shutil
import sys

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ESP_SR = os.path.join(PROJECT, "components", "esp-sr")
MODEL_SRC = os.path.join(ESP_SR, "model")
STAGE = os.path.join(PROJECT, "components", "srmodels_stage")
OUT = os.path.join(PROJECT, "components", "srmodels.bin")

# (model 子目录, 模型名) —— 按需增删模型
SELECTED = [
    ("wakenet_model", "wn9_nihaoxiaozhi"),   # 唤醒词：你好小智
    ("multinet_model", "mn5q8_cn"),          # 中文命令词
]


def main():
    if os.path.exists(STAGE):
        shutil.rmtree(STAGE)
    os.makedirs(STAGE)

    for sub, name in SELECTED:
        src = os.path.join(MODEL_SRC, sub, name)
        dst = os.path.join(STAGE, name)
        if not os.path.isdir(src):
            print("ERROR: model not found: %s" % src)
            sys.exit(1)
        shutil.copytree(src, dst)

    sys.path.insert(0, os.path.join(ESP_SR, "model"))
    from pack_model import pack_models

    pack_models(STAGE, OUT)
    total = os.path.getsize(OUT)
    print("srmodels.bin generated: %d bytes (%.0f KB)" % (total, total / 1024.0))
    print("Recommended model partition size: %d KB" % int(math.ceil(total / 1024.0)))


if __name__ == "__main__":
    main()
