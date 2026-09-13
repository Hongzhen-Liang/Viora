#pragma once
#include <stddef.h>
#include <stdint.h>

// Keep small crypto objects internal. TLS record buffers (16KB each in the
// bundled SDK) belong in PSRAM so lwIP/DMA have internal memory left to use.
template <typename Allocate>
void *tls_memory_allocate(size_t count, size_t size, Allocate allocate) {
  if (size != 0 && count > SIZE_MAX / size) return nullptr;
  if (count * size >= 4096) {
    if (void *p = allocate(count, size, true)) return p;
  }
  return allocate(count, size, false);
}
