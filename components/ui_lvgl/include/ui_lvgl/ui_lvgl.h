#pragma once
#ifndef UI_LVGL_H
#define UI_LVGL_H

#include "sdkconfig.h"

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_lvgl_init(void);
bool ui_lvgl_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_LVGL_H */
