#include "test_runner.h"
#include "ui_lvgl/ui_sensor_model.h"

static void test_capability_measurement_classification(void)
{
    TEST("Device detail: only measurement capabilities get trend data");

    TEST_ASSERT(ui_sensor_capability_has_measurements(
        "DHT11 sensor. Returns 2 values: [temperature_C, humidity_percent]."));
    TEST_ASSERT(ui_sensor_capability_has_measurements(
        "JW01 air sensor: remote_read returns [co2,tvoc,ch2o]."));
    TEST_ASSERT(ui_sensor_capability_has_measurements(
        "Generic ADC Sensor: reads analog voltages"));
    TEST_ASSERT(!ui_sensor_capability_has_measurements(
        "Doorbell: GPIO4 passive buzzer. Use buzzer_beep(id,count)."));
    TEST_ASSERT(!ui_sensor_capability_has_measurements(
        "Servo module: GPIO4 50Hz PWM servo."));
    TEST_ASSERT(!ui_sensor_capability_has_measurements(NULL));
    TEST_PASS();
}

static void test_dht11_metric_metadata(void)
{
    TEST("Sensor detail: DHT11 values receive names and units");

    ui_metric_meta_t temperature = {};
    ui_metric_meta_t humidity = {};
    ui_sensor_metric_meta(
        "DHT11 Temperature and Humidity Sensor: Returns 2 values: "
        "[temperature_C, humidity_percent].",
        0, &temperature);
    ui_sensor_metric_meta(
        "DHT11 Temperature and Humidity Sensor: Returns 2 values: "
        "[temperature_C, humidity_percent].",
        1, &humidity);

    TEST_ASSERT_STR_EQUAL(temperature.label, "Temperature");
    TEST_ASSERT_STR_EQUAL(temperature.unit, "C");
    TEST_ASSERT_STR_EQUAL(humidity.label, "Humidity");
    TEST_ASSERT_STR_EQUAL(humidity.unit, "%");
    TEST_PASS();
}

static void test_binary_and_unknown_metric_metadata(void)
{
    TEST("Sensor detail: binary and unknown capabilities have safe metadata");

    ui_metric_meta_t vibration = {};
    ui_metric_meta_t unknown = {};
    ui_sensor_metric_meta("Vibration Sensor: Returns [vibration_detected]",
                          0, &vibration);
    ui_sensor_metric_meta("custom sensor", 2, &unknown);

    TEST_ASSERT_STR_EQUAL(vibration.label, "Vibration");
    TEST_ASSERT(vibration.binary);
    TEST_ASSERT_STR_EQUAL(unknown.label, "Value 3");
    TEST_ASSERT_STR_EQUAL(unknown.unit, "");
    TEST_ASSERT(!unknown.binary);
    TEST_PASS();
}

static void test_history_keeps_only_successful_recent_values(void)
{
    TEST("Sensor detail: history keeps successful values in the time window");

    ui_sensor_history_t history = {};
    const double first[] = {10.0};
    const double second[] = {20.0};
    const double third[] = {30.0};
    ui_sensor_history_record(&history, 0, first, 1);
    ui_sensor_history_record(&history, 500, NULL, 0); // failed read
    ui_sensor_history_record(&history, 1000, second, 1);
    ui_sensor_history_record(&history, 3000, third, 1);

    float recent[4] = {};
    int count = ui_sensor_history_copy_metric(&history, 0, 3000, 2000,
                                              recent, 4);
    TEST_ASSERT_EQUAL_INT(2, count);
    TEST_ASSERT_EQUAL_DOUBLE(20.0, recent[0], 0.001);
    TEST_ASSERT_EQUAL_DOUBLE(30.0, recent[1], 0.001);
    TEST_PASS();
}

static void test_history_overwrites_oldest_sample_at_capacity(void)
{
    TEST("Sensor detail: history overwrites the oldest sample at capacity");

    ui_sensor_history_t history = {};
    for (int i = 0; i <= UI_SENSOR_HISTORY_CAPACITY; i++) {
        const double value[] = {(double)i};
        ui_sensor_history_record(&history, (uint32_t)i, value, 1);
    }
    float values[UI_SENSOR_HISTORY_CAPACITY] = {};
    const int count = ui_sensor_history_copy_metric(
        &history, 0, UI_SENSOR_HISTORY_CAPACITY,
        UI_SENSOR_HISTORY_CAPACITY, values, UI_SENSOR_HISTORY_CAPACITY);
    TEST_ASSERT_EQUAL_INT(UI_SENSOR_HISTORY_CAPACITY, count);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, values[0], 0.001);
    TEST_ASSERT_EQUAL_DOUBLE((double)UI_SENSOR_HISTORY_CAPACITY,
                             values[count - 1], 0.001);
    TEST_PASS();
}

static void test_selected_module_survives_card_reordering(void)
{
    TEST("Sensor detail: selection follows module id after card reordering");

    const uint8_t reordered_ids[] = {3, 1, 2, 0};
    TEST_ASSERT_EQUAL_INT(
        1, ui_sensor_find_module_index(reordered_ids, 4, 1));
    TEST_ASSERT_EQUAL_INT(
        -1, ui_sensor_find_module_index(reordered_ids, 4, 9));
    TEST_PASS();
}

void test_ui_sensor_model(void)
{
    test_capability_measurement_classification();
    test_dht11_metric_metadata();
    test_binary_and_unknown_metric_metadata();
    test_history_keeps_only_successful_recent_values();
    test_history_overwrites_oldest_sample_at_capacity();
    test_selected_module_survives_card_reordering();
}
