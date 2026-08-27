# Viora OTA 发布手册

Viora 使用 `ota_0` / `ota_1` 双 4MB 分区、HTTPS 下载、
SHA-256 完整性校验和 RSA-3072 离线签名。新固件启动后等待
核心语音模块和 WiFi 稳定 30 秒才确认；崩溃或自检失败会由
bootloader 回滚。

Arduino-ESP32 默认会在 `setup()` 前直接确认 OTA 镜像；项目通过
`verifyRollbackLater()` 把确认延后给 `ota_manager`。删除这个覆盖会使
30 秒健康观察窗失效。

## 一次性迁移（送出设备前）

旧分区表只有一个 app 槽，必须用 USB 完整擦除后重刷。此操作会
清掉 NVS 里的 WiFi，应在交付前执行：

```bash
pio run -e waveshare-esp32-s3-rlcd-42 -t erase
pio run -e waveshare-esp32-s3-rlcd-42 -t upload
```

上传脚本会把 WakeNet 模型写到新的 `0x810000` 分区。上电后在
`http://viora.local/ota` 确认运行槽为 `app0`。

## 配置 HTTPS OTA

1. 把 VioraServer 放在有有效公网证书的 HTTPS 反向代理后。
2. 在被 Git 忽略的 `VioraServer/.env.ota` 配置：

```env
OTA_PUBLIC_BASE_URL=https://ota.example.com
# 每台设备独立 token；device id 是去掉冒号的小写 MAC
OTA_DEVICE_KEYS=aabbccddeeff=token1,112233445566=token2
```

3. 在被 Git 忽略的 `src/ota_secrets.h` 加入：

```cpp
#define SECRET_OTA_MANIFEST_URL "https://ota.example.com/api/firmware/manifest"
#define SECRET_OTA_API_KEY "与服务器对应的 token"
static const char SECRET_OTA_ROOT_CA_VALUE[] = R"PEM(
-----BEGIN CERTIFICATE-----
粘贴签发 HTTPS 证书的根 CA
-----END CERTIFICATE-----
)PEM";
#define SECRET_OTA_ROOT_CA SECRET_OTA_ROOT_CA_VALUE
```

不得使用 `setInsecure()`。`OTA_ROOT_CA` 缺失或 URL 不是 HTTPS 时，
设备会明确显示 `disabled`。

当前部署使用群晖反向代理：

```text
https://sinscry.synology.me:443 -> http://192.168.1.69:11451
```

证书续期只要仍由 Let's Encrypt/ISRG Root X1 信任链签发，设备无需重刷。
Mac 上的 `com.viora.server` LaunchAgent 会在登录后启动服务并在退出时重启；
需保证 T7 磁盘已挂载，并在路由器中为 Mac 保留 `192.168.1.69`。

## 发布新版本

1. 同时增加 `src/config.h` 的 `FIRMWARE_VERSION` 和 `FIRMWARE_BUILD`。
   设备只接受 build 数更大的版本。
2. 构建固件。
3. 用服务器私钥签名并原子切换 manifest：

```bash
pio run -e waveshare-esp32-s3-rlcd-42
VioraServer/.venv/bin/python VioraServer/publish_firmware.py \
  .pio/build/waveshare-esp32-s3-rlcd-42/firmware.bin \
  --version 1.1.0 --build 2
```

发布产物在 `VioraServer/firmware/releases/`。设备开机 60 秒后、每 24 小时或
收到 `{"type":"ota_available"}` 时检查。也可在局域网管理页点击
“立即检查更新”。

## 密钥与恢复

- 设备公钥：`src/ota_public_key.h`。
- 服务器公钥：`VioraServer/firmware/keys/ota_public.pem`。
- 服务器私钥：`VioraServer/firmware/keys/ota_private.pem`（已忽略）。

必须加密备份私钥。私钥丢失后，已送出的设备不会信任新生成的密钥，
只能 USB 召回重刷。私钥泄漏时也不能只换服务器文件，需要设计公钥
轮换版本。
