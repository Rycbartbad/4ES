#include "test_runner.h"
#include "lcd_touch/touch_logic.h"

static void test_finger_count_drives_press_state(void)
{
    TEST("Touch: FingerNum drives press state and panel coordinates are direct");

    const uint8_t registers[] = {
        0x00,       // GestureID
        0x01,       // FingerNum
        0x00, 37,   // X = 37
        0x00, 205,  // Y = 205
    };
    const touch_transform_config_t transform = {
        239, 239, 239, 239, false, false, false,
    };
    touch_decoded_sample_t sample = {};

    TEST_ASSERT(touch_decode_registers(registers, sizeof(registers),
                                       &transform, &sample));
    TEST_ASSERT(sample.pressed);
    TEST_ASSERT_EQUAL_INT(37, sample.x);
    TEST_ASSERT_EQUAL_INT(205, sample.y);
    TEST_PASS();
}

static void test_coordinate_transform(void)
{
    TEST("Touch: coordinates can swap and invert after scaling");

    const uint8_t registers[] = {0x04, 0x01, 0x00, 10, 0x00, 20};
    const touch_transform_config_t transform = {
        239, 239, 239, 239, true, true, false,
    };
    touch_decoded_sample_t sample = {};

    TEST_ASSERT(touch_decode_registers(registers, sizeof(registers),
                                       &transform, &sample));
    TEST_ASSERT_EQUAL_INT(219, sample.x); // swapped Y=20, then invert X
    TEST_ASSERT_EQUAL_INT(10, sample.y);  // swapped X=10
    TEST_ASSERT_EQUAL_INT(0x04, sample.gesture);
    TEST_PASS();
}

static void test_release_scaling_and_clamping(void)
{
    TEST("Touch: release state scales and clamps raw coordinates safely");

    const uint8_t registers[] = {0x00, 0x00, 0x0F, 0xFF, 0x08, 0x00};
    const touch_transform_config_t transform = {
        4095, 4095, 239, 239, false, false, true,
    };
    touch_decoded_sample_t sample = {};
    TEST_ASSERT(touch_decode_registers(registers, sizeof(registers),
                                       &transform, &sample));
    TEST_ASSERT(!sample.pressed);
    TEST_ASSERT_EQUAL_INT(239, sample.x);
    TEST_ASSERT_EQUAL_INT(120, sample.y); // scaled 2048 -> 119, inverted -> 120
    TEST_PASS();
}

void test_touch_logic(void)
{
    test_finger_count_drives_press_state();
    test_coordinate_transform();
    test_release_scaling_and_clamping();
}
