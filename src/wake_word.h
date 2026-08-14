#pragma once

#include <Arduino.h>

// Initialize the Hi Vesper Log-Mel frontend and full-int8 TFLite Micro model.
// Runs a firmware-side golden-vector test before accepting live audio.
bool wake_word_init();

// Feed raw 16 kHz mono PCM. Detection is armed only while `enabled` is true;
// re-arming resets the 1.5-second window to avoid speaker playback leakage.
// The optional probability receives the most recent Hi Vesper probability.
bool wake_word_process(const int16_t *pcm, int samples, bool enabled,
                       float *probability = nullptr);

// Drop all live frontend history and threshold confirmation state.
void wake_word_reset();

// Last measured model invocation time, for device diagnostics.
uint32_t wake_word_last_inference_us();
