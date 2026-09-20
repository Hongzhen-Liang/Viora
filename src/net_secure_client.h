#pragma once

#include <WiFiClientSecure.h>
#include <errno.h>

// Arduino-ESP32 2.x copies connect(timeout) into sslclient->socket_timeout.
// setTimeout() after connecting changes socket options, NOT that TLS deadline.
// Give writes their own bounded budget without lengthening WS reads/handshakes
// or patching the global framework (OTA continues to use the stock client).
class VioraSecureClient : public WiFiClientSecure {
 public:
  explicit VioraSecureClient(uint32_t write_timeout_ms)
      : write_timeout_ms_(write_timeout_ms) {}
  int connectToAddress(IPAddress address, uint16_t port, const char *hostname,
                       int32_t timeout_ms) {
    _timeout = timeout_ms;
    return WiFiClientSecure::connect(address, port, hostname,
                                     _CA_cert, _cert, _private_key);
  }
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  int write_error = 0;
  int write_errno = 0;
  uint32_t write_ms = 0;

  size_t write(const uint8_t *data, size_t length) override {
    if (!_connected || length == 0) return 0;
    const unsigned long previous = sslclient->socket_timeout;
    sslclient->socket_timeout = write_timeout_ms_;
    const uint32_t started = millis();
    errno = 0;
    const int result = send_ssl_data(sslclient, data, length);
    write_errno = result < 0 ? errno : 0;
    write_ms = millis() - started;
    write_error = result < 0 ? result : 0;
    sslclient->socket_timeout = previous;
    if (result < 0) {
      // A partially emitted TLS record cannot be replayed on this socket.
      // Preserve the error before stop() clears its transport state.
      _lastError = result;
      stop();
      return 0;
    }
    return static_cast<size_t>(result);
  }
 private:
  const uint32_t write_timeout_ms_;
};
