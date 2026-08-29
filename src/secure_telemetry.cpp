#include "secure_telemetry.h"

#include <Preferences.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <mbedtls/base64.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

namespace {

constexpr size_t kKeyBytes = 32;
constexpr size_t kIvBytes = 12;
constexpr size_t kTagBytes = 16;
constexpr uint32_t kKeyVersion = 1;

Preferences s_prefs;
bool s_ready = false;
uint8_t s_key[kKeyBytes] = {};
char s_device_id[17] = {};
char s_pairing_code[13] = {};
char s_viewing_key[64] = {};
char s_key_fingerprint[64] = {};
uint32_t s_sequence = 0;

bool encode_base64(const uint8_t *data, size_t len, char *out, size_t out_size) {
  size_t written = 0;
  const int rc = mbedtls_base64_encode(reinterpret_cast<unsigned char *>(out),
                                       out_size - 1, &written, data, len);
  if (rc != 0 || written >= out_size) return false;
  out[written] = '\0';
  return true;
}

void device_id_from_efuse() {
  uint8_t mac[6] = {};
  esp_efuse_mac_get_default(mac);
  snprintf(s_device_id, sizeof(s_device_id), "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void make_pairing_code() {
  static constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  uint8_t random_bytes[8] = {};
  esp_fill_random(random_bytes, sizeof(random_bytes));
  for (size_t i = 0; i < 8; ++i) {
    s_pairing_code[i] = alphabet[random_bytes[i] % (sizeof(alphabet) - 1)];
  }
  s_pairing_code[8] = '\0';
}

bool compute_key_metadata() {
  if (!encode_base64(s_key, sizeof(s_key), s_viewing_key, sizeof(s_viewing_key))) {
    return false;
  }
  uint8_t digest[32] = {};
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  int rc = mbedtls_sha256_starts_ret(&sha, 0);
  if (rc == 0) rc = mbedtls_sha256_update_ret(&sha, s_key, sizeof(s_key));
  if (rc == 0) rc = mbedtls_sha256_finish_ret(&sha, digest);
  mbedtls_sha256_free(&sha);
  if (rc != 0) return false;
  return encode_base64(digest, sizeof(digest), s_key_fingerprint,
                       sizeof(s_key_fingerprint));
}

bool make_aad(uint32_t sequence, uint64_t captured_at, char *out, size_t out_size) {
  const int written = snprintf(out, out_size, "%s\n%u\n%llu\n%u",
                               s_device_id, static_cast<unsigned>(sequence),
                               static_cast<unsigned long long>(captured_at),
                               static_cast<unsigned>(kKeyVersion));
  return written > 0 && static_cast<size_t>(written) < out_size;
}

}  // namespace

bool secure_telemetry_init() {
  if (s_ready) return true;
  device_id_from_efuse();
  if (!s_prefs.begin("viora-sec", false)) {
    Serial.println("[SEC] 无法打开 NVS，遥测加密停用");
    return false;
  }

  const size_t stored = s_prefs.getBytesLength("aes_key");
  if (stored == sizeof(s_key) && s_prefs.getBytes("aes_key", s_key, sizeof(s_key)) == sizeof(s_key)) {
    Serial.printf("[SEC] 已加载设备密钥 device=%s\n", s_device_id);
  } else {
    esp_fill_random(s_key, sizeof(s_key));
    s_prefs.putBytes("aes_key", s_key, sizeof(s_key));
    Serial.printf("[SEC] 首次生成设备密钥 device=%s\n", s_device_id);
    Serial.println("[SEC] 请立即保存下面的 44 位查看码；服务器永远不会收到它：");
  }

  String stored_pairing = s_prefs.getString("pair_code", "");
  if (stored_pairing.length() >= 8 && stored_pairing.length() < sizeof(s_pairing_code)) {
    strlcpy(s_pairing_code, stored_pairing.c_str(), sizeof(s_pairing_code));
  } else {
    make_pairing_code();
    s_prefs.putString("pair_code", s_pairing_code);
  }
  s_sequence = s_prefs.getULong("sequence", 0);
  s_prefs.end();

  if (!compute_key_metadata()) return false;
  Serial.printf("[SEC] device_id=%s pairing_code=%s\n", s_device_id, s_pairing_code);
  Serial.printf("[SEC] viewing_key=%s\n", s_viewing_key);
  Serial.printf("[SEC] key_fingerprint=%s\n", s_key_fingerprint);
  s_ready = true;
  return true;
}

const char *secure_telemetry_device_id() { return s_device_id; }
const char *secure_telemetry_pairing_code() { return s_pairing_code; }
const char *secure_telemetry_key_fingerprint() { return s_key_fingerprint; }
const char *secure_telemetry_viewing_key() { return s_viewing_key; }

uint32_t secure_telemetry_next_sequence() {
  if (!s_ready) return 0;
  ++s_sequence;
  // 每条遥测都保存序号，避免断电后重放同一序号。NVS 自带磨损均衡，
  // 默认 30 秒一条时写入量约为每天 2880 次，远低于常见寿命预算。
  if (s_prefs.begin("viora-sec", false)) {
    s_prefs.putULong("sequence", s_sequence);
    s_prefs.end();
  }
  return s_sequence;
}

bool secure_telemetry_encrypt(const char *plaintext_json,
                              uint32_t sequence,
                              uint64_t captured_at,
                              char *out,
                              size_t out_size) {
  if (!s_ready || !plaintext_json || !out || out_size < 256) return false;
  const size_t plain_len = strlen(plaintext_json);
  if (plain_len == 0 || plain_len > 700) return false;

  uint8_t iv[kIvBytes] = {};
  uint8_t encrypted[700 + kTagBytes] = {};
  esp_fill_random(iv, sizeof(iv));
  char aad[128] = {};
  if (!make_aad(sequence, captured_at, aad, sizeof(aad))) return false;

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s_key, 256);
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plain_len,
                                   iv, sizeof(iv),
                                   reinterpret_cast<const uint8_t *>(aad), strlen(aad),
                                   reinterpret_cast<const uint8_t *>(plaintext_json),
                                   encrypted, kTagBytes, encrypted + plain_len);
  }
  mbedtls_gcm_free(&gcm);
  if (rc != 0) return false;

  char iv_b64[32] = {};
  char ciphertext_b64[1024] = {};
  if (!encode_base64(iv, sizeof(iv), iv_b64, sizeof(iv_b64)) ||
      !encode_base64(encrypted, plain_len + kTagBytes, ciphertext_b64,
                     sizeof(ciphertext_b64))) {
    return false;
  }
  const int written = snprintf(
      out, out_size,
      "{\"type\":\"telemetry_encrypted\",\"device_id\":\"%s\","
      "\"sequence\":%u,\"captured_at\":%llu,\"key_version\":%u,"
      "\"iv\":\"%s\",\"ciphertext\":\"%s\"}",
      s_device_id, static_cast<unsigned>(sequence),
      static_cast<unsigned long long>(captured_at), static_cast<unsigned>(kKeyVersion),
      iv_b64, ciphertext_b64);
  return written > 0 && static_cast<size_t>(written) < out_size;
}

bool secure_telemetry_build_pairing(char *out, size_t out_size) {
  if (!s_ready || !out) return false;
  const int written = snprintf(
      out, out_size,
      "{\"type\":\"device_pairing\",\"device_id\":\"%s\","
      "\"pairing_code\":\"%s\",\"key_fingerprint\":\"%s\","
      "\"key_version\":%u}",
      s_device_id, s_pairing_code, s_key_fingerprint,
      static_cast<unsigned>(kKeyVersion));
  return written > 0 && static_cast<size_t>(written) < out_size;
}
