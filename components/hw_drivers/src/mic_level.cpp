#include "hw_drivers/mic_level.h"

#include <math.h>

double hw_mic_calculate_level_percent(const int32_t* samples,
                                      size_t sample_count)
{
    if (samples == NULL || sample_count == 0) {
        return 0.0;
    }

    int64_t sums[2] = {};
    size_t counts[2] = {};
    for (size_t i = 0; i < sample_count; ++i) {
        const size_t channel = i & 1U;
        sums[channel] += samples[i] >> 8;
        counts[channel]++;
    }

    double square_sums[2] = {};
    for (size_t i = 0; i < sample_count; ++i) {
        const size_t channel = i & 1U;
        const double mean = counts[channel] > 0
            ? (double)sums[channel] / (double)counts[channel]
            : 0.0;
        const double centered = (double)(samples[i] >> 8) - mean;
        square_sums[channel] += centered * centered;
    }

    double max_rms = 0.0;
    for (size_t channel = 0; channel < 2; ++channel) {
        if (counts[channel] == 0) {
            continue;
        }
        const double rms = sqrt(square_sums[channel] / (double)counts[channel]);
        if (rms > max_rms) {
            max_rms = rms;
        }
    }

    double percent = max_rms * 100.0 / 8388608.0;
    if (percent > 100.0) {
        percent = 100.0;
    }
    return percent;
}
