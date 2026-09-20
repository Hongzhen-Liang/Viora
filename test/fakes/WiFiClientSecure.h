#pragma once
#include <stddef.h>
#include <stdint.h>

struct sslclient_context { unsigned long socket_timeout = 5000; };
struct IPAddress {};
uint32_t millis();
int send_ssl_data(sslclient_context *, const uint8_t *, size_t);

class WiFiClientSecure {
 protected:
  sslclient_context storage_;
  sslclient_context *sslclient = &storage_;
  bool _connected = true;
  int _lastError = 0;
  int _timeout = 0;
  const char *_CA_cert = "trusted root", *_cert = nullptr, *_private_key = nullptr;
 public:
  const char *verified_hostname = nullptr;
  int connect(IPAddress, uint16_t, const char *host, const char *ca,
              const char *, const char *) {
    verified_hostname = host;
    return ca != nullptr;
  }
  virtual ~WiFiClientSecure() = default;
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t *, size_t) = 0;
  void stop() { _connected = false; }
  bool connected() const { return _connected; }
  unsigned long read_timeout() const { return sslclient->socket_timeout; }
};
