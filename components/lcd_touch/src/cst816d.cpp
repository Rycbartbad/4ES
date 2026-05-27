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
 *   Touch data is read in a bulk transfer starting at register 0x00.
 *
 * Register map (relevant subset):
 *   Addr  | Name            | R/W | Description
 *   ------+-----------------+-----+------------------------------
 *   0x00  | Status          | R   | Bit[3]: touch detected
 *         |                 |     | Bit[0]: touch active
 *   0x01  | Gesture ID      | R   | Gesture type (see below)
 *   0x02  | Touch points    | R   | Number of current touch points
 *   0x03  | X High          | R   | X[11:4]
 *   0x04  | X Low           | R   | X[3:0] (upper nibble) +
 *         |                 |     | touch 2 Y[7:4] (lower nibble)
 *   0x05  | Y High          | R   | Y[11:4]
 *   0x06  | Y Low           | R   | Y[3:0] (upper nibble) +
 *         |                 |     | touch 2 Y[3:0] / other flags
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

#include "lcd_touch/lcd_touch.h"

/* ====================================================================
 * Local constants
 * ==================================================================== */

static const char* TAG = "cst816d";

/* I2C timeout in FreeRTOS ticks */
#define TOUCH_I2C_TIMEOUT_MS  50

/* CST816D registers */
#define CST816D_REG_STATUS      0x00
#define CST816D_REG_GESTURE     0x01
#define CST816D_REG_TOUCH_NUM   0x02
#define CST816D_REG_X_HIGH      0x03
#define CST816D_REG_X_LOW       0x04
#define CST816D_REG_Y_HIGH      0x05
#define CST816D_REG_Y_LOW       0x06
#define CST816D_REG_SLEEP       0xFE
#define CST816D_REG_VERSION     0xEF

/* Status flags */
#define CST816D_STATUS_TOUCH    (1 << 3)
#define CST816D_STATUS_ACTIVE   (1 << 0)

/* ====================================================================
 * Local state
 * ==================================================================== */

static i2c_master_bus_handle_t   s_i2c_bus   = NULL;
static i2c_master_dev_handle_t   s_i2c_dev   = NULL;
static touch_gesture_t           s_last_gesture = TOUCH_GESTURE_NONE;

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
                        pdMS_TO_TICKS(TOUCH_I2C_TIMEOUT_MS));
    return ret;
}

/**
 * Write a single byte to a touch controller register.
 *
 * @param reg   Register address.
 * @param val   Value to write.
 * @return esp_err_t.
 */
static esp_err_t touch_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_i2c_dev, buf, sizeof(buf),
                               pdMS_TO_TICKS(TOUCH_I2C_TIMEOUT_MS));
}

/* ====================================================================
 * Public API
 * ==================================================================== */

esp_err_t touch_init(void)
{
    ESP_LOGI(TAG, "CST816D init start");

    /* ---- 1. Configure GPIOs ---- */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_TOUCH_RST) |
                        (1ULL << PIN_TOUCH_INT),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OPEN_DRAIN,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* ---- 2. Hardware reset sequence ---- */
    /* Hold RESET low for ≥5 ms, then release and wait for the chip to stabilise */
    gpio_set_level(PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* ---- 3. Initialise I2C bus (I2C_NUM_0) ---- */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port    = TOUCH_I2C_PORT,
        .sda_io_num  = PIN_TOUCH_SDA,
        .scl_io_num  = PIN_TOUCH_SCL,
        .clk_source  = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
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

    /* ---- 4. Wake the touch controller ---- */
    /* Some CST816D modules start in sleep mode.  Writing 0x00 to register
     * 0xFE (sleep control) and to 0xEF (chip ID / status) can wake them. */
    ret = touch_write_reg(CST816D_REG_SLEEP, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wake command failed (may be normal): %s",
                 esp_err_to_name(ret));
    }

    vTaskDelay(pdMS_TO_TICKS(5));

    ret = touch_write_reg(CST816D_REG_VERSION, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Version reg write failed (may be normal): %s",
                 esp_err_to_name(ret));
    }

    /* ---- 5. Configure INT pin as input with pull-up ---- */
    gpio_set_direction(PIN_TOUCH_INT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_TOUCH_INT, GPIO_PULLUP_ONLY);

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

    if (!s_i2c_dev) {
        ESP_LOGW(TAG, "Touch not initialised");
        return ESP_ERR_INVALID_STATE;
    }

    /* Bulk read 7 bytes starting at register 0x00 */
    uint8_t buf[7];
    esp_err_t ret = touch_read_regs(CST816D_REG_STATUS, buf, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ---- Parse status ---- */
    uint8_t status   = buf[0];
    uint8_t gesture  = buf[1];
    uint8_t touch_n  = buf[2];

    /* Gesture update */
    if (gesture != 0) {
        s_last_gesture = (touch_gesture_t)gesture;
        out->gesture   = s_last_gesture;
    } else {
        out->gesture = TOUCH_GESTURE_NONE;
    }

    /* Touch detected? */
    bool touched = (status & CST816D_STATUS_TOUCH) ||
                   (status & CST816D_STATUS_ACTIVE);

    if (!touched || touch_n == 0) {
        out->points = 0;
        return ESP_OK;
    }

    /* ---- Parse touch points ---- */

    /* Point 1: 12-bit X and Y */
    uint16_t x_raw = ((uint16_t)(buf[3] & 0x0F) << 8) | buf[4];
    uint16_t y_raw = ((uint16_t)(buf[5] & 0x0F) << 8) | buf[6];

    /* Scale from 12-bit range (0-4095) to display resolution (240) */
    out->p[0].x = (uint16_t)((uint32_t)x_raw * LCD_WIDTH / 4096);
    out->p[0].y = (uint16_t)((uint32_t)y_raw * LCD_HEIGHT / 4096);
    out->p[0].active = true;

    /* Clamp to valid range */
    if (out->p[0].x >= LCD_WIDTH)  out->p[0].x = LCD_WIDTH - 1;
    if (out->p[0].y >= LCD_HEIGHT) out->p[0].y = LCD_HEIGHT - 1;

    out->points = (touch_n > 0) ? 1 : 0;

    /* Note: CST816D supports up to 2 touch points, but reading the second
     * point requires extended registers.  For V1.0 we support single touch.
     * If touch_n >= 2, we report 1 point and log a debug message. */
    if (touch_n >= 2) {
        ESP_LOGD(TAG, "Second touch point detected but not read (single-touch V1)");
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ */

touch_gesture_t touch_get_gesture(void)
{
    return s_last_gesture;
}

/* ------------------------------------------------------------------ */

void touch_reset(void)
{
    ESP_LOGI(TAG, "Touch reset");
    gpio_set_level(PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
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
