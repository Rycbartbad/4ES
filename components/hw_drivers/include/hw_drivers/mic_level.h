#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Convert interleaved 32-bit I2S slots containing signed 24-bit samples to
// RMS percent of full scale. A per-channel DC offset is removed first.
double hw_mic_calculate_level_percent(const int32_t* samples,
                                      size_t sample_count);

#ifdef __cplusplus
}
#endif
