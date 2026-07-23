#include "test_runner.h"
#include "hw_drivers/mic_level.h"

#include <stdint.h>

static void test_mic_level_rejects_silence_and_dc_offset(void)
{
    TEST("Microphone level: silence and DC offset are zero");
    const int32_t silence[] = {0, 0, 0, 0};
    const int32_t dc_offset[] = {
        100000 << 8, -200000 * 256, 100000 << 8, -200000 * 256,
    };
    TEST_ASSERT_EQUAL_DOUBLE(
        0.0, hw_mic_calculate_level_percent(silence, 4), 0.000001);
    TEST_ASSERT_EQUAL_DOUBLE(
        0.0, hw_mic_calculate_level_percent(dc_offset, 4), 0.000001);
    TEST_PASS();
}

static void test_mic_level_uses_active_i2s_channel(void)
{
    TEST("Microphone level: either I2S slot can carry the microphone");
    const int32_t half_scale_on_right[] = {
        0, 4194304 * 256, 0, -4194304 * 256,
    };
    TEST_ASSERT_EQUAL_DOUBLE(
        50.0,
        hw_mic_calculate_level_percent(half_scale_on_right, 4),
        0.0001);
    TEST_PASS();
}

static void test_mic_level_handles_invalid_input(void)
{
    TEST("Microphone level: empty input is safe");
    TEST_ASSERT_EQUAL_DOUBLE(
        0.0, hw_mic_calculate_level_percent(NULL, 0), 0.000001);
    TEST_PASS();
}

void test_mic_level(void)
{
    test_mic_level_rejects_silence_and_dc_offset();
    test_mic_level_uses_active_i2s_channel();
    test_mic_level_handles_invalid_input();
}
