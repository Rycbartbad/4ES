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

static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t s_draw_buf_1[LCD_WIDTH * UI_DRAW_BUF_ROWS];
static lv_disp_drv_t s_disp_drv;
static esp_timer_handle_t s_tick_timer = NULL;
static TaskHandle_t s_ui_task_handle = NULL;
static TaskHandle_t s_poll_task_handle = NULL;
static bool s_running = false;
static bool s_started = false;

static UiStatusState s_status = {};

typedef struct {
    bool valid;
    uint8_t module_id;
    double values[UI_SENSOR_VALUE_MAX];
    int value_count;
} UiSensorValues;

static UiSensorValues s_sensor_values[UI_SENSOR_CARD_MAX];
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

static void store_sensor_values(uint8_t module_id, const double* values,
                                int value_count)
{
    if (value_count < 0) {
        value_count = 0;
    }
    if (value_count > UI_SENSOR_VALUE_MAX) {
        value_count = UI_SENSOR_VALUE_MAX;
    }

    taskENTER_CRITICAL(&s_sensor_values_lock);
    int slot = -1;
    for (int i = 0; i < UI_SENSOR_CARD_MAX; i++) {
        if (s_sensor_values[i].valid &&
            s_sensor_values[i].module_id == module_id) {
            slot = i;
            break;
        }
        if (slot < 0 && !s_sensor_values[i].valid) {
            slot = i;
        }
    }

    if (slot >= 0) {
        s_sensor_values[slot].valid = true;
        s_sensor_values[slot].module_id = module_id;
        s_sensor_values[slot].value_count = value_count;
        for (int i = 0; i < value_count; i++) {
            s_sensor_values[slot].values[i] = values[i];
        }
    }
    taskEXIT_CRITICAL(&s_sensor_values_lock);
}

int ui_lvgl_copy_sensor_values(uint8_t module_id, double* out_values,
                               int max_values)
{
    if (out_values == NULL || max_values <= 0) {
        return 0;
    }

    int copied = 0;
    taskENTER_CRITICAL(&s_sensor_values_lock);
    for (int i = 0; i < UI_SENSOR_CARD_MAX; i++) {
        if (!s_sensor_values[i].valid ||
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
        break;
    }
    taskEXIT_CRITICAL(&s_sensor_values_lock);
    return copied;
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
        if (count > UI_SENSOR_CARD_MAX) {
            count = UI_SENSOR_CARD_MAX;
        }

        for (int i = 0; i < count; i++) {
            if (peers == NULL || peers[i] == NULL) {
                continue;
            }
            const uint8_t module_id = peers[i]->module_id;
            double values[UI_SENSOR_VALUE_MAX];
            int value_count = espnow_comm_request_read(module_id, values,
                                                       UI_SENSOR_VALUE_MAX);
            store_sensor_values(module_id, values, value_count);
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        vTaskDelay(pdMS_TO_TICKS(UI_SENSOR_POLL_MS));
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
    ESP_LOGI(TAG, "LVGL sensor status UI started");
    return ESP_OK;
}

bool ui_lvgl_is_running(void)
{
    return s_running;
}
