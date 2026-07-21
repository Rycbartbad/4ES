#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_SENSOR_VALUE_MAX 4
#define UI_SENSOR_HISTORY_CAPACITY 120

typedef struct {
    char label[20];
    char unit[8];
    bool binary;
} ui_metric_meta_t;

typedef struct {
    uint32_t timestamp_ms;
    float values[UI_SENSOR_VALUE_MAX];
    uint8_t value_count;
} ui_sensor_history_sample_t;

typedef struct {
    ui_sensor_history_sample_t samples[UI_SENSOR_HISTORY_CAPACITY];
    uint16_t start;
    uint16_t count;
} ui_sensor_history_t;

void ui_sensor_metric_meta(const char* capability, int value_index,
                           ui_metric_meta_t* out);
bool ui_sensor_capability_has_measurements(const char* capability);
void ui_sensor_history_reset(ui_sensor_history_t* history);
void ui_sensor_history_record(ui_sensor_history_t* history,
                              uint32_t timestamp_ms, const double* values,
                              int value_count);
int ui_sensor_history_copy_metric(const ui_sensor_history_t* history,
                                  int value_index, uint32_t now_ms,
                                  uint32_t window_ms, float* out,
                                  int max_values);
int ui_sensor_find_module_index(const uint8_t* module_ids, int count,
                                uint8_t selected_module_id);

#ifdef __cplusplus
}
#endif
