#!/usr/bin/env python3
"""
一键恢复 ESP-SR 组件（全新克隆后使用）。

components/ 已在 .gitignore 中忽略，执行本脚本会：
  1. 下载 esp-sr 1.9.2（匹配 Arduino core 2.0.17 / ESP-IDF 4.4）
  2. 下载 esp-dsp 1.4.0（esp-sr 的依赖，<=1.5.0）
  3. 解压到 components/esp-sr、components/esp-dsp
  4. 把 esp-sr 的 esp32s3 静态库合并为 libespsr.a（解决链接顺序/互相引用）

完成后运行 platformio run 即可构建。
需要网络；需要已安装 PlatformIO espressif32 平台（含 xtensa-esp32s3 工具链）。
"""
import os
import shutil
import subprocess
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMP = os.path.join(ROOT, "components")
ESP_SR = os.path.join(COMP, "esp-sr")
ESP_DSP = os.path.join(COMP, "esp-dsp")

ESP_SR_URL = ("https://components-file.espressif.com/components/espressif/esp-sr/"
              "1.9.2/espressif__esp-sr-v1.9.2.zip")
ESP_DSP_URL = ("https://components-file.espressif.com/components/espressif/esp-dsp/"
               "1.4.0/espressif__esp-dsp-v1.4.0.zip")


def download(url, dest):
    if os.path.exists(dest) and os.path.getsize(dest) > 10000:
        print("已存在: %s" % dest)
        return
    print("下载 %s" % url)
    urllib.request.urlretrieve(url, dest)


def extract(zip_path, dest):
    if os.path.isdir(dest):
        print("已存在: %s" % dest)
        return
    print("解压 %s -> %s" % (zip_path, dest))
    shutil.unpack_archive(zip_path, dest)


def find_ar():
    cands = [
        os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-ar"),
        shutil.which("xtensa-esp32s3-elf-ar"),
    ]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    raise RuntimeError("未找到 xtensa-esp32s3-elf-ar，请先安装 PlatformIO espressif32 平台")


def merge_libs():
    lib_dir = os.path.join(ESP_SR, "lib", "esp32s3")
    merged = os.path.join(ESP_SR, "lib", "esp32s3_merged")
    out = os.path.join(merged, "libespsr.a")
    if os.path.isfile(out):
        print("已存在: %s" % out)
        return
    ar = find_ar()
    tmp = os.path.join(merged, "_m")
    if os.path.isdir(tmp):
        shutil.rmtree(tmp)
    os.makedirs(tmp)
    i = 0
    for name in sorted(os.listdir(lib_dir)):
        if not name.endswith(".a"):
            continue
        d = os.path.join(tmp, "d%d" % i)
        os.makedirs(d)
        subprocess.check_call([ar, "x", os.path.join(lib_dir, name)], cwd=d)
        i += 1
    files = []
    for root, _, fs in os.walk(tmp):
        for f in fs:
            files.append(os.path.join(root, f))
    subprocess.check_call([ar, "rcs", out] + files)
    shutil.rmtree(tmp)
    print("已生成合并库: %s" % out)


def main():
    os.makedirs(COMP, exist_ok=True)
    download(ESP_SR_URL, os.path.join(COMP, "esp-sr-1.9.2.zip"))
    download(ESP_DSP_URL, os.path.join(COMP, "esp-dsp-1.4.0.zip"))
    extract(os.path.join(COMP, "esp-sr-1.9.2.zip"), ESP_SR)
    extract(os.path.join(COMP, "esp-dsp-1.4.0.zip"), ESP_DSP)
    merge_libs()
    print("完成。现在可以运行 platformio run 构建。")


if __name__ == "__main__":
    main()
