#include <assert.h>
#include <errno.h>
#include <string.h>
#include "../src/net_secure_client.h"

static uint32_t now_ms = 0;
static unsigned long observed_timeout;
static int send_result;
uint32_t millis() { return now_ms; }
int send_ssl_data(sslclient_context *context, const uint8_t *, size_t) {
  observed_timeout = context->socket_timeout;
  now_ms += 6100;  // a successful write after the former five-second deadline
  errno = EAGAIN;
  return send_result;
}

int main() {
  VioraSecureClient client(15000);
  assert(client.connectToAddress(IPAddress{}, 11451, "voice.example.test", 5000));
  assert(strcmp(client.verified_hostname, "voice.example.test") == 0);
  const uint8_t audio[] = {1, 2, 3, 4};
  send_result = sizeof(audio);
  assert(client.write(audio, sizeof(audio)) == sizeof(audio));
  assert(observed_timeout == 15000 && client.connected());
  assert(client.read_timeout() == 5000); // receiving keeps its original budget
  assert(client.write_error == 0 && client.write_errno == 0);
  assert(client.write_ms == 6100);
  send_result = -1;
  assert(client.write(audio, sizeof(audio)) == 0);
  assert(client.write_error == -1 && client.write_errno == EAGAIN);
  assert(client.read_timeout() == 5000 && !client.connected());
  const auto finished = now_ms;
  assert(client.write(audio, sizeof(audio)) == 0 && now_ms == finished);
  VioraSecureClient fatal(15000);
  send_result = -0x1234;
  assert(fatal.write(uint8_t(1)) == 0);
  assert(fatal.write_error == -0x1234 && !fatal.connected());
}
