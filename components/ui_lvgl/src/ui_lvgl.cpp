#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "espnow_comm/comm.h"
#include "espnow_comm/peer_mgr.h"
#include "lcd_touch/lcd_touch.h"
#include "ui_lvgl/ui_lvgl.h"
#include "ui_lvgl_internal.h"

static const char* TAG = "ui_lvgl";

static constexpr uint32_t UI_DRAW_BUF_ROWS = 20;
static constexpr uint32_t UI_TASK_DELAY_MS = 5;
static constexpr uint32_t UI_REFRESH_MS = 250;
static constexpr uint32_t UI_PEER_REFRESH_MS = 500;
static constexpr uint32_t UI_SENSOR_POLL_MS = 2000;
static constexpr uint32_t UI_SELECTED_SENSOR_POLL_MS = 1000;

static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t s_draw_buf_1[LCD_WIDTH * UI_DRAW_BUF_ROWS];
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_indev_drv;
static lv_indev_t* s_touch_indev = NULL;
static esp_timer_handle_t s_tick_timer = NULL;
static TaskHandle_t s_ui_task_handle = NULL;
static TaskHandle_t s_poll_task_handle = NULL;
static bool s_running = false;
static bool s_started = false;

static UiStatusState s_status = {};

typedef struct {
    bool in_use;
    uint8_t module_id;
    double values[UI_SENSOR_VALUE_MAX];
    int value_count;
    uint32_t last_update_ms;
    uint32_t last_poll_ms;
    ui_sensor_history_t history;
} UiSensorValues;

static UiSensorValues s_sensor_values[UI_DEVICE_MAX];
static portMUX_TYPE s_sensor_values_lock = portMUX_INITIALIZER_UNLOCKED;

static void lcd_write_lvgl_colors(const lv_color_t* colors, uint32_t count)
{
#if LV_COLOR_DEPTH == 16 && !LV_COLOR_16_SWAP
    lcd_write_pixels(reinterpret_cast<const uint16_t*>(colors), count);
#else
    uint16_t tx_buf[128];
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = (count - done > 128) ? 128 : (count - done);
        for (uint32_t i = 0; i < chunk; i++) {
            tx_buf[i] = lv_color_to16(colors[done + i]).full;
        }
        lcd_write_pixels(tx_buf, chunk);
        done += chunk;
    }
#endif
}

static void disp_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area,
                          lv_color_t* color_p)
{
    int32_t x0 = area->x1;
    int32_t y0 = area->y1;
    int32_t x1 = area->x2;
    int32_t y1 = area->y2;

    if (x1 < 0 || y1 < 0 || x0 >= LCD_WIDTH || y0 >= LCD_HEIGHT) {
        lv_disp_flush_ready(drv);
        return;
    }

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= LCD_WIDTH) x1 = LCD_WIDTH - 1;
    if (y1 >= LCD_HEIGHT) y1 = LCD_HEIGHT - 1;

    const uint32_t area_w = (uint32_t)lv_area_get_width(area);
    const uint32_t clipped_w = (uint32_t)(x1 - x0 + 1);
    const uint32_t clipped_h = (uint32_t)(y1 - y0 + 1);
    const uint32_t x_offset = (uint32_t)(x0 - area->x1);
    const uint32_t y_offset = (uint32_t)(y0 - area->y1);

    lcd_set_window((uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1);

    if (clipped_w == area_w) {
        const uint32_t pixel_offset = y_offset * area_w + x_offset;
        lcd_write_lvgl_colors(color_p + pixel_offset, clipped_w * clipped_h);
    } else {
        for (uint32_t row = 0; row < clipped_h; row++) {
            const uint32_t pixel_offset =
                (y_offset + row) * area_w + x_offset;
            lcd_write_lvgl_colors(color_p + pixel_offset, clipped_w);
        }
    }

    lv_disp_flush_ready(drv);
}

static void lv_tick_cb(void* arg)
{
    (void)arg;
    lv_tick_inc(1);
}

static void touch_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data)
{
    (void)drv;
    static lv_point_t last_point = {0, 0};

    data->point = last_point;
    data->state = LV_INDEV_STATE_RELEASED;
    if (!touch_is_initialized()) {
        return;
    }

    touch_data_t touch = {};
    if (touch_read(&touch) != ESP_OK || touch.points == 0 ||
        !touch.p[0].active) {
        return;
    }

    last_point.x = (lv_coord_t)touch.p[0].x;
    last_point.y = (lv_coord_t)touch.p[0].y;
    data->point = last_point;
    data->state = LV_INDEV_STATE_PRESSED;
}

static void store_sensor_values(uint8_t module_id, const double* values,
                                int value_count)
{
    if (values == NULL || value_count <= 0) {
        return;
    }
    if (value_count > UI_SENSOR_VALUE_MAX) {
        value_count = UI_SENSOR_VALUE_MAX;
    }

    taskENTER_CRITICAL(&s_sensor_values_lock);
    int slot = -1;
    for (int i = 0; i < UI_DEVICE_MAX; i++) {
        if (s_sensor_values[i].in_use &&
            s_sensor_values[i].module_id == module_id) {
            slot = i;
            break;
        }
        if (slot < 0 && !s_sensor_values[i].in_use) {
            slot = i;
        }
    }

    if (slot >= 0) {
        s_sensor_values[slot].in_use = true;
        s_sensor_values[slot].module_id = module_id;
        s_sensor_values[slot].value_count = value_count;
        s_sensor_values[slot].last_update_ms =
            (uint32_t)(esp_timer_get_time() / 1000);
        for (int i = 0; i < value_count; i++) {
            s_sensor_values[slot].values[i] = values[i];
        }
        ui_sensor_history_record(&s_sensor_values[slot].history,
                                 s_sensor_values[slot].last_update_ms,
                                 values, value_count);
    }
    taskEXIT_CRITICAL(&s_sensor_values_lock);
}

int ui_lvgl_copy_sensor_values(uint8_t module_id, double* out_values,
                               int max_values)
{
    return ui_lvgl_copy_sensor_snapshot(module_id, out_values, max_values,
                                        NULL);
}

int ui_lvgl_copy_sensor_snapshot(uint8_t module_id, double* out_values,
                                 int max_values, uint32_t* last_update_ms)
{
    if (out_values == NULL || max_values <= 0) {
        return 0;
    }

    int copied = 0;
    taskENTER_CRITICAL(&s_sensor_values_lock);
    for (int i = 0; i < UI_DEVICE_MAX; i++) {
        if (!s_sensor_values[i].in_use ||
            s_sensor_values[i].module_id != module_id) {
            continue;
        }

        copied = s_sensor_values[i].value_count;
        if (copied > max_values) {
            copied = max_values;
        }
        for (int j = 0; j < copied; j++) {
            out_values[j] = s_sensor_values[i].values[j];
        }
        if (last_update_ms != NULL) {
            *last_update_ms = s_sensor_values[i].last_update_ms;
        }
        break;
    }
    taskEXIT_CRITICAL(&s_sensor_values_lock);
    return copied;
}

int ui_lvgl_copy_sensor_history(uint8_t module_id, int value_index,
                                uint32_t now_ms, uint32_t window_ms,
                                float* out, int max_values)
{
    int copied = 0;
    taskENTER_CRITICAL(&s_sensor_values_lock);
    for (int i = 0; i < UI_DEVICE_MAX; i++) {
        if (s_sensor_values[i].in_use &&
            s_sensor_values[i].module_id == module_id) {
            copied = ui_sensor_history_copy_metric(
                &s_sensor_values[i].history, value_index, now_ms, window_ms,
                out, max_values);
            break;
        }
    }
    taskEXIT_CRITICAL(&s_sensor_values_lock);
    return copied;
}

static bool claim_sensor_poll(uint8_t module_id, uint32_t now_ms,
                              uint32_t interval_ms)
{
    bool due = false;
    taskENTER_CRITICAL(&s_sensor_values_lock);
    int slot = -1;
    for (int i = 0; i < UI_DEVICE_MAX; i++) {
        if (s_sensor_values[i].in_use &&
            s_sensor_values[i].module_id == module_id) {
            slot = i;
            break;
        }
        if (slot < 0 && !s_sensor_values[i].in_use) {
            slot = i;
        }
    }
    if (slot >= 0) {
        UiSensorValues* sensor = &s_sensor_values[slot];
        if (!sensor->in_use) {
            sensor->in_use = true;
            sensor->module_id = module_id;
            sensor->last_poll_ms = now_ms - interval_ms;
        }
        if ((uint32_t)(now_ms - sensor->last_poll_ms) >= interval_ms) {
            sensor->last_poll_ms = now_ms;
            due = true;
        }
    }
    taskEXIT_CRITICAL(&s_sensor_values_lock);
    return due;
}

static void ui_task(void* arg)
{
    (void)arg;

    TickType_t last_ui_update = 0;
    TickType_t last_peer_update = 0;

    while (1) {
        lv_timer_handler();

        TickType_t now = xTaskGetTickCount();
        if (now - last_peer_update >= pdMS_TO_TICKS(UI_PEER_REFRESH_MS)) {
            ui_peer_view_refresh(&s_status);
            last_peer_update = now;
        }
        if (now - last_ui_update >= pdMS_TO_TICKS(UI_REFRESH_MS)) {
            ui_screen_diag_update(&s_status);
            last_ui_update = now;
        }

        vTaskDelay(pdMS_TO_TICKS(UI_TASK_DELAY_MS));
    }
}

static void sensor_poll_task(void* arg)
{
    (void)arg;

    while (1) {
        int count = 0;
        PeerEntry** peers = peer_mgr_list(&count);
        if (count > UI_DEVICE_MAX) {
            count = UI_DEVICE_MAX;
        }

        const uint8_t selected_module = ui_screen_diag_selected_module();
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        bool requested = false;
        for (int i = 0; i < count; i++) {
            if (peers == NULL || peers[i] == NULL) {
                continue;
            }
            if (peers[i]->state != PEER_ACTIVE) {
                continue;
            }
            if (!ui_sensor_capability_has_measurements(peers[i]->capability)) {
                continue;
            }
            const uint8_t module_id = peers[i]->module_id;
            const uint32_t interval_ms =
                (module_id == selected_module) ? UI_SELECTED_SENSOR_POLL_MS
                                               : UI_SENSOR_POLL_MS;
            if (!claim_sensor_poll(module_id, now_ms, interval_ms)) {
                continue;
            }
            double values[UI_SENSOR_VALUE_MAX];
            int value_count = espnow_comm_request_read(module_id, values,
                                                       UI_SENSOR_VALUE_MAX);
            // Only update cached values on a successful read.
            // When espnow_comm_request_read returns 0 (concurrent request
            // rejected or genuine timeout), preserve the last good reading
            // so the UI does not flash zeros.
            if (value_count > 0) {
                store_sensor_values(module_id, values, value_count);
            }
            requested = true;
            break; // Keep ESP-NOW requests strictly serial.
        }
        vTaskDelay(pdMS_TO_TICKS(requested ? 20 : 50));
    }
}

esp_err_t ui_lvgl_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    lv_init();

    lv_disp_draw_buf_init(&s_draw_buf, s_draw_buf_1, NULL,
                          LCD_WIDTH * UI_DRAW_BUF_ROWS);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LCD_WIDTH;
    s_disp_drv.ver_res = LCD_HEIGHT;
    s_disp_drv.flush_cb = disp_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;

    lv_disp_t* disp = lv_disp_drv_register(&s_disp_drv);
    if (disp == NULL) {
        ESP_LOGW(TAG, "LVGL display registration failed");
        return ESP_FAIL;
    }

    if (touch_is_initialized()) {
        lv_indev_drv_init(&s_indev_drv);
        s_indev_drv.type = LV_INDEV_TYPE_POINTER;
        s_indev_drv.read_cb = touch_read_cb;
        s_touch_indev = lv_indev_drv_register(&s_indev_drv);
        if (s_touch_indev == NULL) {
            ESP_LOGW(TAG, "LVGL touch input registration failed");
        }
    } else {
        ESP_LOGW(TAG, "Touch unavailable; LVGL display remains active");
    }

    ui_peer_view_refresh(&s_status);
    ui_screen_diag_create(&s_status);
    ui_screen_diag_update(&s_status);

    esp_timer_create_args_t tick_args = {};
    tick_args.callback = lv_tick_cb;
    tick_args.name = "lv_tick";

    esp_err_t ret = esp_timer_create(&tick_args, &s_tick_timer);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LVGL tick timer create failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = esp_timer_start_periodic(s_tick_timer, 1000);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LVGL tick timer start failed: %s",
                 esp_err_to_name(ret));
        esp_timer_delete(s_tick_timer);
        s_tick_timer = NULL;
        return ret;
    }

    BaseType_t task_ret = xTaskCreate(ui_task, "ui_lvgl", 6144, NULL, 2,
                                      &s_ui_task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGW(TAG, "Failed to create UI task");
        esp_timer_stop(s_tick_timer);
        esp_timer_delete(s_tick_timer);
        s_tick_timer = NULL;
        return ESP_ERR_NO_MEM;
    }

    task_ret = xTaskCreate(sensor_poll_task, "ui_poll", 4096, NULL, 1,
                           &s_poll_task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGW(TAG, "Failed to create UI sensor poll task");
    }

    s_started = true;
    s_running = true;

    // Register sensor read callback so read_sensor() in scripts
    // returns the same cached values shown on the LCD.
    extern int (*g_sensor_read_callback)(uint8_t module_id, double* out, int max);
    g_sensor_read_callback = ui_lvgl_copy_sensor_values;

    ESP_LOGI(TAG, "LVGL sensor status UI started");
    return ESP_OK;
}

bool ui_lvgl_is_running(void)
{
    return s_running;
}
