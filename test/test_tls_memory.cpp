#include "../src/tls_memory_policy.h"
#include <assert.h>
#include <initializer_list>
#include <stdlib.h>
#include <string.h>

int main() {
  // Small crypto allocations stay internal; large buffers choose PSRAM.
  for (size_t bytes : {size_t(32), size_t(4095), size_t(4096), size_t(16384)}) {
    unsigned calls = 0;
    void *p = tls_memory_allocate(1, bytes, [&](size_t n, size_t s, bool external) {
      ++calls;
      assert(external == (bytes >= 4096));
      return calloc(n, s);
    });
    assert(p && calls == 1);
    for (size_t i = 0; i < bytes; ++i) assert(static_cast<unsigned char *>(p)[i] == 0);
    free(p);
  }
  unsigned calls = 0;
  void *p = tls_memory_allocate(2, 8192, [&](size_t n, size_t s, bool external) -> void * {
    assert(external == (calls++ == 0));
    return external ? nullptr : calloc(n, s);
  });
  assert(p && calls == 2); free(p);
  calls = 0;
  p = tls_memory_allocate(SIZE_MAX, 2, [&](size_t, size_t, bool) -> void * {
    ++calls; return nullptr;
  });
  assert(!p && calls == 0);
  p = tls_memory_allocate(1, 16384, [](size_t, size_t, bool) -> void * { return nullptr; });
  assert(!p);
}
