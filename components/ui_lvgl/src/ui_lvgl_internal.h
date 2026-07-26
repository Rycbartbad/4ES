#pragma once
#ifndef UI_LVGL_INTERNAL_H
#define UI_LVGL_INTERNAL_H

#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>

#include "ui_lvgl/ui_sensor_model.h"

#define UI_DEVICE_MAX CONFIG_MAX_PEERS
#define UI_SENSOR_NAME_LEN 17
#define UI_SENSOR_DATA_LEN 48
#define UI_SENSOR_CAPABILITY_LEN 129

typedef struct {
    bool present;
    bool connected;
    uint8_t module_id;
    char name[UI_SENSOR_NAME_LEN];
    char capability[UI_SENSOR_CAPABILITY_LEN];
    char data[UI_SENSOR_DATA_LEN];
    double values[UI_SENSOR_VALUE_MAX];
    int value_count;
    uint32_t last_update_ms;
} UiDeviceCard;

typedef struct {
    int total_device_count;
    int device_count;
    UiDeviceCard devices[UI_DEVICE_MAX];
} UiStatusState;

void ui_screen_diag_create(UiStatusState* state);
void ui_screen_diag_update(const UiStatusState* state);
void ui_screen_diag_navigate_back(void);
void ui_peer_view_refresh(UiStatusState* state);
int ui_lvgl_copy_sensor_values(uint8_t module_id, double* out_values,
                               int max_values);
int ui_lvgl_copy_sensor_snapshot(uint8_t module_id, double* out_values,
                                 int max_values, uint32_t* last_update_ms);
int ui_lvgl_copy_sensor_history(uint8_t module_id, int value_index,
                                uint32_t now_ms, uint32_t window_ms,
                                float* out, int max_values);
uint8_t ui_screen_diag_selected_module(void);

#endif /* UI_LVGL_INTERNAL_H */
