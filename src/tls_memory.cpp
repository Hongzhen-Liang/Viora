#include "tls_memory.h"
#include "tls_memory_policy.h"
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

static void *tls_calloc(size_t count, size_t size) {
  return tls_memory_allocate(count, size, [](size_t n, size_t s, bool external) {
    return heap_caps_calloc(n, s, MALLOC_CAP_8BIT |
        (external ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL));
  });
}

bool tls_memory_init() {
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) return false;
  // Both pools are released by heap_caps_free, including any earlier objects
  // created by the SDK's default allocator. Never switch hooks mid-session.
  return mbedtls_platform_set_calloc_free(tls_calloc, heap_caps_free) == 0;
}
