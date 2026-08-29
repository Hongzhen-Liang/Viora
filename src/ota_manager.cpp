#include "ota_manager.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <time.h>

#include "config.h"
#include "net.h"
#include "ota_public_key.h"

// Arduino-ESP32 默认会在 setup() 之前立刻确认新 OTA 镜像。覆盖其弱符号，
// 将确认延后到 Viora 完成硬件/语音自检并稳定联网 30 秒之后。
extern "C" bool verifyRollbackLater() { return true; }

namespace {

struct ReleaseInfo {
  String version;
  uint32_t build = 0;
  String hardware;
  uint32_t model_version = 0;
  size_t size = 0;
  String sha256;
  String signature;
  String url;
};

Preferences s_prefs;
bool s_initialized = false;
bool s_busy = false;
bool s_manual_check = false;
bool s_download_requested = false;
bool s_install_requested = false;
bool s_pending_verify = false;
bool s_core_health_ok = false;
bool s_time_sync_started = false;
bool s_update_available = false;
bool s_ready_to_install = false;
uint32_t s_started_ms = 0;
uint32_t s_next_check_ms = OTA_INITIAL_CHECK_MS;
char s_status[24] = "idle";
char s_error[128] = "";
ReleaseInfo s_available_release;
String s_prepared_slot;
OtaUiCallback s_ui_callback = nullptr;

bool elapsed(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

void persist(const char *status, const char *error = "") {
  strlcpy(s_status, status ? status : "unknown", sizeof(s_status));
  strlcpy(s_error, error ? error : "", sizeof(s_error));
  if (s_prefs.begin("viora_ota", false)) {
    s_prefs.putString("status", s_status);
    s_prefs.putString("error", s_error);
    s_prefs.putString("version", FIRMWARE_VERSION);
    s_prefs.putUInt("build", FIRMWARE_BUILD);
    const esp_partition_t *running = esp_ota_get_running_partition();
    s_prefs.putString("slot", running ? running->label : "unknown");
    s_prefs.end();
  }
  Serial.printf("[OTA] status=%s%s%s\n", s_status,
                s_error[0] ? " error=" : "", s_error);
}

void fail(const String &message) {
  persist("failed", message.c_str());
  if (s_ui_callback) {
    s_ui_callback(OtaUiEvent::kError, s_available_release.version.c_str(), 0);
  }
  s_next_check_ms = millis() + OTA_RETRY_INTERVAL_MS;
}

void notify_ui(OtaUiEvent event, uint32_t value = 0) {
  if (s_ui_callback) {
    s_ui_callback(event, s_available_release.version.c_str(), value);
  }
}

void clear_prepared_release() {
  s_ready_to_install = false;
  s_prepared_slot = "";
  if (s_prefs.begin("viora_ota", false)) {
    s_prefs.remove("prepared");
    s_prefs.remove("prep_slot");
    s_prefs.remove("prep_ver");
    s_prefs.remove("prep_build");
    s_prefs.end();
  }
}

void save_prepared_release(const ReleaseInfo &release,
                           const esp_partition_t *target) {
  s_ready_to_install = target != nullptr;
  s_prepared_slot = target ? target->label : "";
  if (s_prefs.begin("viora_ota", false)) {
    s_prefs.putBool("prepared", s_ready_to_install);
    s_prefs.putString("prep_slot", s_prepared_slot);
    s_prefs.putString("prep_ver", release.version);
    s_prefs.putUInt("prep_build", release.build);
    s_prefs.end();
  }
}

void restore_prepared_release() {
  if (!s_prefs.begin("viora_ota", true)) return;
  const bool prepared = s_prefs.getBool("prepared", false);
  const uint32_t build = prepared ? s_prefs.getUInt("prep_build", 0) : 0;
  const String slot = prepared ? s_prefs.getString("prep_slot", "") : "";
  const String version = prepared ? s_prefs.getString("prep_ver", "") : "";
  s_prefs.end();
  if (!prepared || build <= FIRMWARE_BUILD || slot.isEmpty() ||
      version.isEmpty()) {
    if (prepared) clear_prepared_release();
    return;
  }
  const esp_partition_t *target = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, slot.c_str());
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (!target || target == running) {
    clear_prepared_release();
    return;
  }
  s_available_release.version = version;
  s_available_release.build = build;
  s_prepared_slot = slot;
  s_update_available = true;
  s_ready_to_install = true;
}

bool config_ready() {
  return OTA_MANIFEST_URL[0] != '\0' && OTA_ROOT_CA[0] != '\0' &&
         strncmp(OTA_MANIFEST_URL, "https://", 8) == 0 &&
         strstr(OTA_ROOT_CA, "BEGIN CERTIFICATE") != nullptr &&
         strstr(OTA_ROOT_CA, "replace-with-your-root-ca") == nullptr;
}

void add_auth_headers(HTTPClient &http) {
  if (OTA_API_KEY[0] != '\0') http.addHeader("X-Ota-Key", OTA_API_KEY);
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char device_id[13];
  snprintf(device_id, sizeof(device_id), "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  http.addHeader("X-Device-Id", device_id);
  http.addHeader("X-Firmware-Version", FIRMWARE_VERSION);
  http.addHeader("X-Firmware-Build", String(FIRMWARE_BUILD));
}

bool parse_manifest(const String &payload, ReleaseInfo &release, String &error) {
  JsonDocument doc;
  const DeserializationError json_error = deserializeJson(doc, payload);
  if (json_error) {
    error = String("manifest JSON: ") + json_error.c_str();
    return false;
  }
  release.version = String(doc["version"] | "");
  release.build = doc["build"] | 0U;
  release.hardware = String(doc["hardware"] | "");
  release.model_version = doc["model_version"] | 0U;
  release.size = doc["size"] | 0U;
  release.sha256 = String(doc["sha256"] | "");
  release.signature = String(doc["signature"] | "");
  release.url = String(doc["url"] | "");

  if (release.version.isEmpty() || release.build == 0 || release.size == 0 ||
      release.sha256.length() != 64 || release.signature.isEmpty()) {
    error = "manifest 字段不完整";
    return false;
  }
  if (release.hardware != FIRMWARE_HARDWARE) {
    error = "硬件型号不匹配";
    return false;
  }
  if (release.model_version != FIRMWARE_MODEL_VERSION) {
    error = "WakeNet 模型版本不匹配";
    return false;
  }
  if (!release.url.startsWith("https://")) {
    error = "固件 URL 不是 HTTPS";
    return false;
  }
  return true;
}

bool hex_digest(const String &hex, uint8_t out[32]) {
  if (hex.length() != 64) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < 32; ++i) {
    const int hi = nibble(hex[i * 2]);
    const int lo = nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool verify_signature(const uint8_t digest[32], const String &encoded,
                      String &error) {
  size_t signature_len = 0;
  const size_t capacity = (encoded.length() * 3U) / 4U + 4U;
  uint8_t *signature = static_cast<uint8_t *>(malloc(capacity));
  if (!signature) {
    error = "签名校验内存不足";
    return false;
  }
  int rc = mbedtls_base64_decode(signature, capacity, &signature_len,
                                 reinterpret_cast<const uint8_t *>(encoded.c_str()),
                                 encoded.length());
  if (rc != 0) {
    free(signature);
    error = "签名 Base64 无效";
    return false;
  }

  mbedtls_pk_context public_key;
  mbedtls_pk_init(&public_key);
  rc = mbedtls_pk_parse_public_key(
      &public_key, reinterpret_cast<const uint8_t *>(OTA_SIGNING_PUBLIC_KEY),
      strlen(OTA_SIGNING_PUBLIC_KEY) + 1);
  if (rc == 0) {
    rc = mbedtls_pk_verify(&public_key, MBEDTLS_MD_SHA256, digest, 32,
                           signature, signature_len);
  }
  mbedtls_pk_free(&public_key);
  free(signature);
  if (rc != 0) {
    error = "RSA 签名校验失败";
    return false;
  }
  return true;
}

bool fetch_manifest(ReleaseInfo &release, String &error) {
  WiFiClientSecure client;
  client.setCACert(OTA_ROOT_CA);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  if (!http.begin(client, OTA_MANIFEST_URL)) {
    error = "无法初始化 manifest HTTPS";
    return false;
  }
  add_auth_headers(http);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    error = String("manifest HTTP ") + code;
    http.end();
    return false;
  }
  const int content_length = http.getSize();
  if (content_length <= 0 || content_length > 8192) {
    error = "manifest 大小异常";
    http.end();
    return false;
  }
  const String payload = http.getString();
  http.end();
  return parse_manifest(payload, release, error);
}

bool download_release(const ReleaseInfo &release,
                      const esp_partition_t *&downloaded_partition,
                      String &error) {
  downloaded_partition = nullptr;
  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  if (!target) {
    error = "未找到非活动 OTA 分区";
    return false;
  }
  if (release.size > target->size) {
    error = "固件超过 OTA 分区大小";
    return false;
  }

  WiFiClientSecure client;
  client.setCACert(OTA_ROOT_CA);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  if (!http.begin(client, release.url)) {
    error = "无法初始化固件 HTTPS";
    return false;
  }
  add_auth_headers(http);
  const int code = http.GET();
  if (code != HTTP_CODE_OK || http.getSize() != static_cast<int>(release.size)) {
    error = code != HTTP_CODE_OK ? String("固件 HTTP ") + code
                                 : "固件 Content-Length 不匹配";
    http.end();
    return false;
  }

  esp_ota_handle_t handle = 0;
  esp_err_t ota_error = esp_ota_begin(target, release.size, &handle);
  if (ota_error != ESP_OK) {
    error = String("esp_ota_begin: ") + esp_err_to_name(ota_error);
    http.end();
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buffer[4096];
  size_t written = 0;
  uint8_t last_percent = 255;
  bool write_ok = true;
  while (written < release.size) {
    const size_t remaining = release.size - written;
    const size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    const size_t got = stream->readBytes(buffer, wanted);
    if (got == 0) {
      write_ok = false;
      error = "固件下载中断";
      break;
    }
    ota_error = esp_ota_write(handle, buffer, got);
    if (ota_error != ESP_OK) {
      write_ok = false;
      error = String("esp_ota_write: ") + esp_err_to_name(ota_error);
      break;
    }
    mbedtls_sha256_update_ret(&sha, buffer, got);
    written += got;
    const uint8_t percent = static_cast<uint8_t>(
        (static_cast<uint64_t>(written) * 100U) / release.size);
    if (percent != last_percent) {
      last_percent = percent;
      notify_ui(OtaUiEvent::kDownloadProgress, percent);
    }
    if ((written & 0xFFFFU) < got) {
      Serial.printf("[OTA] 下载 %u/%u bytes\n", static_cast<unsigned>(written),
                    static_cast<unsigned>(release.size));
    }
    delay(1);
  }
  http.end();

  uint8_t actual_digest[32];
  mbedtls_sha256_finish_ret(&sha, actual_digest);
  mbedtls_sha256_free(&sha);
  if (!write_ok) {
    esp_ota_abort(handle);
    return false;
  }

  uint8_t expected_digest[32];
  if (!hex_digest(release.sha256, expected_digest) ||
      memcmp(actual_digest, expected_digest, sizeof(actual_digest)) != 0) {
    esp_ota_abort(handle);
    error = "固件 SHA-256 不匹配";
    return false;
  }
  if (!verify_signature(actual_digest, release.signature, error)) {
    esp_ota_abort(handle);
    return false;
  }

  ota_error = esp_ota_end(handle);
  if (ota_error != ESP_OK) {
    error = String("esp_ota_end: ") + esp_err_to_name(ota_error);
    return false;
  }
  downloaded_partition = target;
  return true;
}

void check_now() {
  s_busy = true;
  persist("checking");
  ReleaseInfo release;
  String error;
  if (!fetch_manifest(release, error)) {
    fail(error);
    s_busy = false;
    return;
  }
  if (release.build <= FIRMWARE_BUILD) {
    s_update_available = s_ready_to_install;
    if (!s_ready_to_install) s_available_release = ReleaseInfo{};
    persist("up_to_date");
    notify_ui(OtaUiEvent::kUpToDate);
    s_next_check_ms = millis() + OTA_CHECK_INTERVAL_MS;
    s_busy = false;
    return;
  }
  s_available_release = release;
  s_update_available = true;
  Serial.printf("[OTA] 发现新版本 %s build=%lu\n", release.version.c_str(),
                static_cast<unsigned long>(release.build));
  persist("available");
  notify_ui(OtaUiEvent::kAvailable, release.build);
  s_next_check_ms = millis() + OTA_CHECK_INTERVAL_MS;
  s_busy = false;
}

void download_now() {
  if (!s_update_available || s_available_release.build <= FIRMWARE_BUILD) {
    fail("没有可下载的新版本");
    s_busy = false;
    return;
  }
  s_busy = true;
  persist("downloading");
  notify_ui(OtaUiEvent::kDownloading);
  String error;
  const esp_partition_t *target = nullptr;
  if (!download_release(s_available_release, target, error)) {
    fail(error);
    s_busy = false;
    return;
  }
  save_prepared_release(s_available_release, target);
  persist("ready_to_install");
  notify_ui(OtaUiEvent::kReadyToInstall, s_available_release.build);
  Serial.printf("[OTA] 固件已验证并保存到 %s，等待用户确认安装\n",
                target ? target->label : "unknown");
  s_busy = false;
}

void install_now() {
  const esp_partition_t *target = s_prepared_slot.isEmpty()
                                      ? nullptr
                                      : esp_partition_find_first(
                                            ESP_PARTITION_TYPE_APP,
                                            ESP_PARTITION_SUBTYPE_ANY,
                                            s_prepared_slot.c_str());
  if (!s_ready_to_install || !target ||
      s_available_release.build <= FIRMWARE_BUILD) {
    fail("没有已下载、可安装的固件");
    s_busy = false;
    return;
  }
  s_busy = true;
  const esp_err_t ota_error = esp_ota_set_boot_partition(target);
  if (ota_error != ESP_OK) {
    fail(String("设置启动分区: ") + esp_err_to_name(ota_error));
    s_busy = false;
    return;
  }
  persist("rebooting");
  notify_ui(OtaUiEvent::kInstalling, s_available_release.build);
  Serial.println("[OTA] 用户已确认安装，即将重启验证");
  delay(500);
  ESP.restart();
}

}  // namespace

void ota_init(bool core_health_ok) {
  s_initialized = true;
  s_core_health_ok = core_health_ok;
  s_started_ms = millis();
  s_next_check_ms = s_started_ms + OTA_INITIAL_CHECK_MS;
  restore_prepared_release();

  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    s_pending_verify = true;
    persist("validating");
    Serial.printf("[OTA] 待验证固件 %s build=%u slot=%s\n", FIRMWARE_VERSION,
                  FIRMWARE_BUILD, running->label);
    if (!s_core_health_ok) {
      persist("rollback", "核心硬件或语音模块自检失败");
      delay(500);
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  } else if (s_ready_to_install) {
    persist("ready_to_install");
    notify_ui(OtaUiEvent::kReadyToInstall, s_available_release.build);
  } else {
    persist(config_ready() ? "idle" : "disabled");
  }
}

void ota_loop(bool idle) {
  if (!s_initialized) return;
  if (WiFi.status() == WL_CONNECTED && !s_time_sync_started) {
    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
    s_time_sync_started = true;
  }
  if (s_pending_verify) {
    if (s_core_health_ok && WiFi.status() == WL_CONNECTED &&
        millis() - s_started_ms >= OTA_VALIDATION_MS) {
      const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
      if (result == ESP_OK) {
        s_pending_verify = false;
        persist("valid");
      } else {
        fail(String("确认 OTA 固件失败: ") + esp_err_to_name(result));
      }
    }
    return;
  }
  if (s_busy || !idle) return;
  if (s_install_requested) {
    s_install_requested = false;
    install_now();
    return;
  }
  // 已验证的镜像保持不变，直到用户明确安装；期间不再用更新检查覆盖元数据。
  if (s_ready_to_install) return;
  if (!config_ready() || WiFi.status() != WL_CONNECTED) return;
  if (time(nullptr) < 1700000000) return;  // TLS 证书校验需要正确时间
  if (s_download_requested) {
    s_download_requested = false;
    net_release_tls_for_ota();
    download_now();
    return;
  }
  const uint32_t now = millis();
  if (!s_manual_check && !elapsed(now, s_next_check_ms)) return;
  s_manual_check = false;
  net_release_tls_for_ota();
  check_now();
}

void ota_request_check() {
  if (config_ready()) s_manual_check = true;
}

void ota_request_download() {
  if (config_ready() && s_update_available && !s_ready_to_install) {
    s_download_requested = true;
  }
}

void ota_request_install() {
  if (s_ready_to_install) s_install_requested = true;
}

void ota_set_ui_callback(OtaUiCallback callback) { s_ui_callback = callback; }

bool ota_enabled() { return config_ready(); }
bool ota_busy() { return s_busy; }
bool ota_update_available() { return s_update_available; }
bool ota_ready_to_install() { return s_ready_to_install; }
const char *ota_available_version() {
  return s_available_release.version.c_str();
}
uint32_t ota_available_build() { return s_available_release.build; }
const char *ota_status() { return s_status; }
const char *ota_last_error() { return s_error; }

const char *ota_running_slot() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  return running ? running->label : "unknown";
}
