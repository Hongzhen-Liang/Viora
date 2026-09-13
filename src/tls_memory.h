#pragma once

// Install once during setup, before any network/TLS tasks can allocate.
bool tls_memory_init();
