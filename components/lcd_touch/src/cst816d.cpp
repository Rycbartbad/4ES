/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * CST816D Capacitive Touch Controller Driver (I2C)
 *
 * Protocol:
 *   7-bit I2C address: 0x15
 *   Supports fast mode (400 kHz).
 *   Touch data is read in a bulk transfer starting at register 0x01.
 *
 * Register map (relevant subset):
 *   Addr  | Name            | R/W | Description
 *   ------+-----------------+-----+------------------------------
 *   0x01  | Gesture ID      | R   | Gesture type (see below)
 *   0x02  | Touch points    | R   | Number of current touch points
 *   0x03  | X High          | R   | X[11:8] in low nibble
 *   0x04  | X Low           | R   | X[7:0]
 *   0x05  | Y High          | R   | Y[11:8] in low nibble
 *   0x06  | Y Low           | R   | Y[7:0]
 *
 * Gesture IDs:
 *   0x00 = none       0x01 = swipe up     0x02 = swipe down
 *   0x03 = swipe left  0x04 = swipe right  0x05 = click
 *   0x0B = double click  0x0C = long press
 */

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include <cstring>

#include "lcd_touch/lcd_touch.h"
#include "lcd_touch/touch_logic.h"

/* ====================================================================
 * Local constants
 * ==================================================================== */

static const char* TAG = "cst816d";

/* I2C timeout in FreeRTOS ticks */
#define TOUCH_I2C_TIMEOUT_MS  50

/* CST816D registers */
#define CST816D_REG_GESTURE     0x01

#ifndef CONFIG_LCD_TOUCH_RAW_MAX_X
#define CONFIG_LCD_TOUCH_RAW_MAX_X 239
#endif
#ifndef CONFIG_LCD_TOUCH_RAW_MAX_Y
#define CONFIG_LCD_TOUCH_RAW_MAX_Y 239
#endif

static const touch_transform_config_t TOUCH_TRANSFORM = {
    .raw_max_x = CONFIG_LCD_TOUCH_RAW_MAX_X,
    .raw_max_y = CONFIG_LCD_TOUCH_RAW_MAX_Y,
    .panel_max_x = LCD_WIDTH - 1,
    .panel_max_y = LCD_HEIGHT - 1,
#ifdef CONFIG_LCD_TOUCH_SWAP_XY
    .swap_xy = true,
#else
    .swap_xy = false,
#endif
#ifdef CONFIG_LCD_TOUCH_INVERT_X
    .invert_x = true,
#else
    .invert_x = false,
#endif
#ifdef CONFIG_LCD_TOUCH_INVERT_Y
    .invert_y = true,
#else
    .invert_y = false,
#endif
};

/* ====================================================================
 * Local state
 * ==================================================================== */

static i2c_master_bus_handle_t   s_i2c_bus   = NULL;
static i2c_master_dev_handle_t   s_i2c_dev   = NULL;
static touch_gesture_t           s_last_gesture = TOUCH_GESTURE_NONE;
static bool                      s_touch_ready = false;

/* ====================================================================
 * Low-level I2C helpers
 * ==================================================================== */

/**
 * Read a block of registers from the touch controller.
 *
 * @param reg   Starting register address.
 * @param data  Buffer to receive the data.
 * @param len   Number of bytes to read.
 * @return esp_err_t.
 */
static esp_err_t touch_read_regs(uint8_t reg, uint8_t* data, uint32_t len)
{
    /* Write the register address, then read back */
    esp_err_t ret = i2c_master_transmit_receive(
                        s_i2c_dev,
                        &reg, 1,         /* write: register address */
                        data, len,       /* read: register data */
                        TOUCH_I2C_TIMEOUT_MS);
    return ret;
}

/* ====================================================================
 * Public API
 * ==================================================================== */

esp_err_t touch_init(void)
{
    ESP_LOGI(TAG, "CST816D init start");
    s_touch_ready = false;

    /* ---- 1. Configure optional GPIOs ---- */
#if PIN_TOUCH_RST >= 0
    gpio_config_t rst_conf = {
        .pin_bit_mask = (1ULL << PIN_TOUCH_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_conf);
#endif

#if PIN_TOUCH_INT >= 0
    gpio_config_t int_conf = {
        .pin_bit_mask = (1ULL << PIN_TOUCH_INT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_conf);
#endif

    /* ---- 2. Hardware reset sequence ---- */
    /* Hold RESET low for ≥5 ms, then release and wait for the chip to stabilise */
#if PIN_TOUCH_RST >= 0
    gpio_set_level(PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
#else
    ESP_LOGI(TAG, "No touch RESET pin; using power-on reset");
    vTaskDelay(pdMS_TO_TICKS(200));
#endif

    /* ---- 3. Initialise I2C bus (I2C_NUM_0) ---- */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port    = TOUCH_I2C_PORT,
        .sda_io_num  = PIN_TOUCH_SDA,
        .scl_io_num  = PIN_TOUCH_SCL,
        .clk_source  = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    bus_cfg.flags.enable_internal_pullup = true;
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_master_probe(s_i2c_bus, TOUCH_I2C_ADDR,
                           TOUCH_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CST816D not found at 0x%02X: %s",
                 TOUCH_I2C_ADDR, esp_err_to_name(ret));
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length    = I2C_ADDR_BIT_LEN_7,
        .device_address     = TOUCH_I2C_ADDR,
        .scl_speed_hz       = TOUCH_I2C_CLOCK_HZ,
    };
    ret = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(ret));
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        return ret;
    }

    /* ---- 4. Configure optional INT; LVGL otherwise polls over I2C ---- */
#if PIN_TOUCH_INT >= 0
    gpio_set_direction(PIN_TOUCH_INT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_TOUCH_INT, GPIO_PULLUP_ONLY);
#else
    ESP_LOGI(TAG, "No touch INT pin; using polling mode");
#endif

    s_touch_ready = true;
    ESP_LOGI(TAG, "CST816D init done");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

esp_err_t touch_read(touch_data_t* out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    if (!s_touch_ready || !s_i2c_dev) {
        ESP_LOGW(TAG, "Touch not initialised");
        return ESP_ERR_INVALID_STATE;
    }

    /* GestureID, FingerNum, X and Y are contiguous at 0x01..0x06. */
    uint8_t buf[6];
    esp_err_t ret = touch_read_regs(CST816D_REG_GESTURE, buf, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "I2C read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    touch_decoded_sample_t sample = {};
    if (!touch_decode_registers(buf, sizeof(buf), &TOUCH_TRANSFORM, &sample)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Gesture update */
    if (sample.gesture != 0) {
        s_last_gesture = (touch_gesture_t)sample.gesture;
        out->gesture   = s_last_gesture;
    } else {
        out->gesture = TOUCH_GESTURE_NONE;
    }

    /* Touch detected? */
    if (!sample.pressed) {
        out->points = 0;
        return ESP_OK;
    }

    out->p[0].x = sample.x;
    out->p[0].y = sample.y;
    out->p[0].active = true;
    out->points = 1;

    return ESP_OK;
}

/* ------------------------------------------------------------------ */

touch_gesture_t touch_get_gesture(void)
{
    return s_last_gesture;
}

/* ------------------------------------------------------------------ */

bool touch_is_initialized(void)
{
    return s_touch_ready;
}

/* ------------------------------------------------------------------ */

void touch_reset(void)
{
    ESP_LOGI(TAG, "Touch reset");
#if PIN_TOUCH_RST >= 0
    gpio_set_level(PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
#else
    ESP_LOGW(TAG, "Touch RESET pin is not connected; reset skipped");
#endif
    s_last_gesture = TOUCH_GESTURE_NONE;
}

/* ====================================================================
 * Combined Init (lcd_touch.h)
 * ==================================================================== */

esp_err_t lcd_touch_init(void)
{
    esp_err_t ret;

    /* 1. Initialise LCD (SPI bus + display init) */
    ret = lcd_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD init failed, aborting");
        return ret;
    }

    /* 2. Initialise touch (I2C bus + touch init) */
    ret = touch_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch init failed, continuing without touch");
        /* LCD can still be used without touch — not a fatal error */
    }

    return ESP_OK;
}
