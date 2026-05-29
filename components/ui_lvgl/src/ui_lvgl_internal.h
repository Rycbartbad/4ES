#pragma once
#ifndef UI_LVGL_INTERNAL_H
#define UI_LVGL_INTERNAL_H

#include "sdkconfig.h"

#include <stdbool.h>

#define UI_SENSOR_CARD_MAX 4
#define UI_SENSOR_NAME_LEN 17
#define UI_SENSOR_DATA_LEN 48

typedef struct {
    bool present;
    bool connected;
    char name[UI_SENSOR_NAME_LEN];
    char data[UI_SENSOR_DATA_LEN];
} UiSensorCard;

typedef struct {
    int total_sensor_count;
    UiSensorCard sensors[UI_SENSOR_CARD_MAX];
} UiStatusState;

void ui_screen_diag_create(UiStatusState* state);
void ui_screen_diag_update(const UiStatusState* state);
void ui_peer_view_refresh(UiStatusState* state);

#endif /* UI_LVGL_INTERNAL_H */
