/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO V1.0 -- offline sensor bring-up firmware.
 *
 * This entry point is intentionally local-only: it initializes the selected
 * sensor and prints compact key-value readings to the serial monitor without
 * starting Wi-Fi, ESP-NOW, peer discovery, or announce tasks.
 */

#include "sdkconfig.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"

#include "hw_drivers/drivers.h"

// ====================================================================
// Offline sensor selection
// Set exactly one USE_SENSOR_* macro to 1.
// ====================================================================

#define USE_SENSOR_DHT11     0
#define USE_SENSOR_VIBRATION 0
#define USE_SENSOR_RAINDROP  0
#define USE_SENSOR_BH1750    0
#define USE_SENSOR_JW01      0
#define USE_SENSOR_ADC_RAW   0
#define USE_SENSOR_PRESSURE  1

#define OFFLINE_SENSOR_TASK_STACK_BYTES 3072
#define OFFLINE_SENSOR_TASK_PRIORITY    5
#define OFFLINE_SENSOR_LOG_INTERVAL_MS  50

static const char* TAG = "offline_sensor";

#if (USE_SENSOR_DHT11 + USE_SENSOR_VIBRATION + USE_SENSOR_RAINDROP + \
     USE_SENSOR_BH1750 + USE_SENSOR_JW01 + USE_SENSOR_ADC_RAW + \
     USE_SENSOR_PRESSURE) != 1
#error "Set exactly one USE_SENSOR_* macro to 1"
#endif

// ====================================================================
// DHT11 sensor
// ====================================================================

#if USE_SENSOR_DHT11

#define DHT11_PIN GPIO_NUM_13
#define DHT11_PIN_SCAN_ENABLED 1
#define DHT11_RESPONSE_TIMEOUT_US 100
#define DHT11_BIT_TIMEOUT_US      120
#define DHT11_ONE_THRESHOLD_US    40

typedef enum {
    DHT11_STATUS_OK = 0,
    DHT11_STATUS_BAD_ARG,
    DHT11_STATUS_NO_RESPONSE_LOW,
    DHT11_STATUS_RESPONSE_LOW_TIMEOUT,
    DHT11_STATUS_RESPONSE_HIGH_TIMEOUT,
    DHT11_STATUS_BIT_LOW_TIMEOUT,
    DHT11_STATUS_BIT_HIGH_TIMEOUT,
    DHT11_STATUS_CHECKSUM_FAIL,
} dht11_status_t;

static const char* dht11_status_name(dht11_status_t status)
{
    switch (status) {
    case DHT11_STATUS_OK: return "OK";
    case DHT11_STATUS_BAD_ARG: return "BAD_ARG";
    case DHT11_STATUS_NO_RESPONSE_LOW: return "NO_RESPONSE_LOW";
    case DHT11_STATUS_RESPONSE_LOW_TIMEOUT: return "RESPONSE_LOW_TIMEOUT";
    case DHT11_STATUS_RESPONSE_HIGH_TIMEOUT: return "RESPONSE_HIGH_TIMEOUT";
    case DHT11_STATUS_BIT_LOW_TIMEOUT: return "BIT_LOW_TIMEOUT";
    case DHT11_STATUS_BIT_HIGH_TIMEOUT: return "BIT_HIGH_TIMEOUT";
    case DHT11_STATUS_CHECKSUM_FAIL: return "CHECKSUM_FAIL";
    default: return "UNKNOWN";
    }
}

static bool wait_for_level(gpio_num_t pin, int level, int timeout_us)
{
    int waited = 0;
    while (gpio_get_level(pin) != level) {
        if (waited >= timeout_us) {
            return false;
        }
        esp_rom_delay_us(1);
        waited++;
    }
    return true;
}

static int measure_level_width(gpio_num_t pin, int level, int timeout_us)
{
    int width = 0;
    while (gpio_get_level(pin) == level) {
        if (width >= timeout_us) {
            return -1;
        }
        esp_rom_delay_us(1);
        width++;
    }
    return width;
}

static esp_err_t dht11_init_pin(gpio_num_t pin)
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;

    return gpio_config(&cfg);
}

static esp_err_t dht11_init(void)
{
    return dht11_init_pin(DHT11_PIN);
}

static dht11_status_t dht11_read_pin(gpio_num_t pin,
                                     double* temperature_c,
                                     double* humidity_pct)
{
    if (temperature_c == NULL || humidity_pct == NULL) {
        return DHT11_STATUS_BAD_ARG;
    }

    uint8_t data[5] = {0, 0, 0, 0, 0};

    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(pin, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);

    if (!wait_for_level(pin, 0, DHT11_RESPONSE_TIMEOUT_US)) {
        return DHT11_STATUS_NO_RESPONSE_LOW;
    }
    if (measure_level_width(pin, 0, DHT11_RESPONSE_TIMEOUT_US) < 0) {
        return DHT11_STATUS_RESPONSE_LOW_TIMEOUT;
    }
    if (measure_level_width(pin, 1, DHT11_RESPONSE_TIMEOUT_US) < 0) {
        return DHT11_STATUS_RESPONSE_HIGH_TIMEOUT;
    }

    for (int bit = 0; bit < 40; bit++) {
        if (!wait_for_level(pin, 1, DHT11_BIT_TIMEOUT_US)) {
            return DHT11_STATUS_BIT_LOW_TIMEOUT;
        }

        const int high_width_us = measure_level_width(pin, 1, DHT11_BIT_TIMEOUT_US);
        if (high_width_us < 0) {
            return DHT11_STATUS_BIT_HIGH_TIMEOUT;
        }

        data[bit / 8] <<= 1;
        if (high_width_us > DHT11_ONE_THRESHOLD_US) {
            data[bit / 8] |= 1;
        }
    }

    const uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (checksum != data[4]) {
        return DHT11_STATUS_CHECKSUM_FAIL;
    }

    *humidity_pct = (double)data[0] + ((double)data[1] / 10.0);
    *temperature_c = (double)data[2] + ((double)data[3] / 10.0);
    return DHT11_STATUS_OK;
}

static dht11_status_t dht11_read(double* temperature_c, double* humidity_pct)
{
    return dht11_read_pin(DHT11_PIN, temperature_c, humidity_pct);
}

#if DHT11_PIN_SCAN_ENABLED
static const gpio_num_t s_dht11_scan_pins[] = {
    GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_8,
    GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13,
    GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_18,
    GPIO_NUM_21, GPIO_NUM_33, GPIO_NUM_34, GPIO_NUM_35, GPIO_NUM_36,
    GPIO_NUM_37, GPIO_NUM_38, GPIO_NUM_39, GPIO_NUM_40, GPIO_NUM_41,
    GPIO_NUM_42, GPIO_NUM_47, GPIO_NUM_48,
};
#endif

#endif

// ====================================================================
// Digital GPIO sensors
// ====================================================================

#if USE_SENSOR_VIBRATION

#define VIBRATION_PIN GPIO_NUM_6

static esp_err_t vibration_init(void)
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << VIBRATION_PIN;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;

    return gpio_config(&cfg);
}

#endif

#if USE_SENSOR_RAINDROP

#define RAINDROP_PIN GPIO_NUM_6

static esp_err_t raindrop_init(void)
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << RAINDROP_PIN;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;

    return gpio_config(&cfg);
}

#endif

// ====================================================================
// BH1750 light sensor
// ====================================================================

#if USE_SENSOR_BH1750

#define I2C_MASTER_SCL_IO  GPIO_NUM_22
#define I2C_MASTER_SDA_IO  GPIO_NUM_21
#define I2C_MASTER_NUM     I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 100000
#define BH1750_SENSOR_ADDR 0x23
#define BH1750_CMD_START   0x20

static esp_err_t bh1750_init(void)
{
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_MASTER_SDA_IO;
    conf.scl_io_num = I2C_MASTER_SCL_IO;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_MASTER_FREQ_HZ;
    conf.clk_flags = 0;

    esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return ret;
}

static esp_err_t bh1750_write_command(uint8_t command)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BH1750_SENSOR_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, command, true);
    i2c_master_stop(cmd);

    const esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t bh1750_read_bytes(uint8_t* data, size_t data_size)
{
    if (data == NULL || data_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BH1750_SENSOR_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &data[0], I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &data[1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    const esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t bh1750_read(double* lux, uint16_t* raw, const char** fail_status)
{
    if (lux == NULL || raw == NULL || fail_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *fail_status = "WRITE_FAIL";
    esp_err_t ret = bh1750_write_command(BH1750_CMD_START);
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(180));

    uint8_t data[2] = {0, 0};
    *fail_status = "READ_FAIL";
    ret = bh1750_read_bytes(data, sizeof(data));
    if (ret != ESP_OK) {
        return ret;
    }

    *raw = (uint16_t)((data[0] << 8) | data[1]);
    *lux = (double)(*raw) / 1.2;
    *fail_status = "OK";
    return ESP_OK;
}

#endif

// ====================================================================
// JW01 gas sensor
// ====================================================================

#if USE_SENSOR_JW01

#define JW01_UART_NUM       UART_NUM_1
#define JW01_TX_PIN         GPIO_NUM_16
#define JW01_RX_PIN         GPIO_NUM_15
#define JW01_BAUD_RATE      9600
#define JW01_RAW_LOG_BYTES  16
#define JW01_RAW_HEX_CHARS  ((JW01_RAW_LOG_BYTES * 3) + 1)

static esp_err_t jw01_init(void)
{
    uart_config_t uart_config = {};
    uart_config.baud_rate = JW01_BAUD_RATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    esp_err_t ret = uart_param_config(JW01_UART_NUM, &uart_config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = uart_set_pin(JW01_UART_NUM, JW01_TX_PIN, JW01_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = uart_driver_install(JW01_UART_NUM, 256, 0, 0, NULL, 0);
    if (ret == ESP_FAIL) {
        return ESP_OK;
    }
    return ret;
}

static void format_hex_bytes(const uint8_t* data, int length, char* out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (data == NULL || length <= 0) {
        return;
    }

    int used = 0;
    const int count = (length < JW01_RAW_LOG_BYTES) ? length : JW01_RAW_LOG_BYTES;
    for (int i = 0; i < count; i++) {
        const int written = snprintf(&out[used], out_size - (size_t)used,
                                     "%s%02X", (i == 0) ? "" : ":", data[i]);
        if (written < 0 || (size_t)written >= out_size - (size_t)used) {
            out[out_size - 1] = '\0';
            return;
        }
        used += written;
    }
}

#endif

// ====================================================================
// ADC raw fallback
// ====================================================================

#if USE_SENSOR_ADC_RAW

#define SENSOR_ADC_COUNT 3

static const uint8_t s_adc_pins[SENSOR_ADC_COUNT] = {4, 5, 6};

#endif

// ====================================================================
// Pressure sensor module: AO/DO/GND/VCC
// ====================================================================

#if USE_SENSOR_PRESSURE

#define PRESSURE_AO_PIN GPIO_NUM_4
#define PRESSURE_DO_PIN GPIO_NUM_6

static esp_err_t pressure_init(void)
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << PRESSURE_DO_PIN;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;

    return gpio_config(&cfg);
}

#endif

// ====================================================================
// Offline sensor facade
// ====================================================================

static const char* offline_sensor_name(void)
{
#if USE_SENSOR_DHT11
    return "DHT11";
#elif USE_SENSOR_VIBRATION
    return "VIBRATION";
#elif USE_SENSOR_RAINDROP
    return "RAINDROP";
#elif USE_SENSOR_BH1750
    return "BH1750";
#elif USE_SENSOR_JW01
    return "JW01";
#elif USE_SENSOR_ADC_RAW
    return "ADC_RAW";
#elif USE_SENSOR_PRESSURE
    return "PRESSURE_AO_DO";
#endif
}

static esp_err_t offline_sensor_init(void)
{
#if USE_SENSOR_DHT11
    return dht11_init();
#elif USE_SENSOR_VIBRATION
    return vibration_init();
#elif USE_SENSOR_RAINDROP
    return raindrop_init();
#elif USE_SENSOR_BH1750
    return bh1750_init();
#elif USE_SENSOR_JW01
    return jw01_init();
#elif USE_SENSOR_ADC_RAW
    return ESP_OK;
#elif USE_SENSOR_PRESSURE
    return pressure_init();
#endif
}

static void offline_sensor_log_reading(void)
{
#if USE_SENSOR_DHT11
#if DHT11_PIN_SCAN_ENABLED
    bool found = false;
    for (size_t i = 0; i < sizeof(s_dht11_scan_pins) / sizeof(s_dht11_scan_pins[0]); i++) {
        double temperature_c = 0.0;
        double humidity_pct = 0.0;
        const gpio_num_t pin = s_dht11_scan_pins[i];

        dht11_init_pin(pin);
        const dht11_status_t status = dht11_read_pin(pin, &temperature_c, &humidity_pct);
        if (status == DHT11_STATUS_OK) {
            found = true;
            ESP_LOGI(TAG, "sensor=DHT11_SCAN status=FOUND gpio=%d temp_c=%.1f humidity_pct=%.1f",
                     (int)pin, temperature_c, humidity_pct);
        } else {
            ESP_LOGW(TAG, "sensor=DHT11_SCAN status=MISS gpio=%d reason=%s",
                     (int)pin, dht11_status_name(status));
        }
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    if (!found) {
        ESP_LOGW(TAG, "sensor=DHT11_SCAN status=NO_PIN_FOUND");
    }
#else
    double temperature_c = 0.0;
    double humidity_pct = 0.0;
    const dht11_status_t status = dht11_read(&temperature_c, &humidity_pct);

    if (status == DHT11_STATUS_OK) {
        ESP_LOGI(TAG, "sensor=DHT11 status=OK temp_c=%.1f humidity_pct=%.1f",
                 temperature_c, humidity_pct);
    } else {
        ESP_LOGW(TAG, "sensor=DHT11 status=READ_FAIL reason=%s",
                 dht11_status_name(status));
    }
#endif
#elif USE_SENSOR_VIBRATION
    const int level = gpio_get_level(VIBRATION_PIN);
    ESP_LOGI(TAG, "sensor=VIBRATION status=OK gpio=%d raw_level=%d state=%s",
             (int)VIBRATION_PIN, level, level ? "active" : "inactive");
#elif USE_SENSOR_RAINDROP
    const int level = gpio_get_level(RAINDROP_PIN);
    ESP_LOGI(TAG, "sensor=RAINDROP status=OK gpio=%d raw_level=%d state=%s",
             (int)RAINDROP_PIN, level, level ? "wet" : "dry");
#elif USE_SENSOR_BH1750
    double lux = 0.0;
    uint16_t raw = 0;
    const char* fail_status = "READ_FAIL";
    const esp_err_t ret = bh1750_read(&lux, &raw, &fail_status);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "sensor=BH1750 status=OK raw=%u lux=%.2f",
                 (unsigned int)raw, lux);
    } else {
        ESP_LOGW(TAG, "sensor=BH1750 status=%s err=0x%x",
                 fail_status, (unsigned int)ret);
    }
#elif USE_SENSOR_JW01
    uint8_t data[64] = {};
    const int length = uart_read_bytes(JW01_UART_NUM, data, sizeof(data), pdMS_TO_TICKS(100));
    char raw_hex[JW01_RAW_HEX_CHARS] = {};

    if (length <= 0) {
        ESP_LOGW(TAG, "sensor=JW01 status=NO_BYTES len=%d", length);
        return;
    }

    format_hex_bytes(data, length, raw_hex, sizeof(raw_hex));
    if (length < 9) {
        ESP_LOGW(TAG, "sensor=JW01 status=SHORT_FRAME len=%d raw_hex=%s",
                 length, raw_hex);
        return;
    }

    const uint16_t co2_raw = (uint16_t)((data[1] << 8) | data[2]);
    const uint16_t tvoc_raw = (uint16_t)((data[3] << 8) | data[4]);
    const uint16_t ch2o_raw = (uint16_t)((data[5] << 8) | data[6]);

    ESP_LOGI(TAG, "sensor=JW01 status=OK len=%d raw_hex=%s co2_raw=%u tvoc_raw=%u ch2o_raw=%u",
             length, raw_hex, (unsigned int)co2_raw,
             (unsigned int)tvoc_raw, (unsigned int)ch2o_raw);
#elif USE_SENSOR_ADC_RAW
    int values[SENSOR_ADC_COUNT] = {};
    for (int i = 0; i < SENSOR_ADC_COUNT; i++) {
        values[i] = hw_adc_read(s_adc_pins[i]);
    }

    ESP_LOGI(TAG, "sensor=ADC_RAW status=OK gpio4_raw=%d gpio5_raw=%d gpio6_raw=%d",
             values[0], values[1], values[2]);
#elif USE_SENSOR_PRESSURE
    const int ao_raw = hw_adc_read((uint8_t)PRESSURE_AO_PIN);
    const int do_level = gpio_get_level(PRESSURE_DO_PIN);

    ESP_LOGI(TAG, "sensor=PRESSURE_AO_DO status=OK ao_gpio=%d ao_raw=%d do_gpio=%d do_level=%d do_state=%s",
             (int)PRESSURE_AO_PIN, ao_raw,
             (int)PRESSURE_DO_PIN, do_level,
             do_level ? "high" : "low");
#endif
}

// ====================================================================
// Offline task and app entry
// ====================================================================

static void offline_sensor_task(void* arg)
{
    (void)arg;

    while (true) {
        offline_sensor_log_reading();
        vTaskDelay(pdMS_TO_TICKS(OFFLINE_SENSOR_LOG_INTERVAL_MS));
    }
}

extern "C" void app_main(void)
{
    printf("ESP-LEGO V1.0 offline sensor firmware starting...\n");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ret = offline_sensor_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sensor=%s status=INIT_FAIL err=0x%x",
                 offline_sensor_name(), (unsigned int)ret);
    } else {
        ESP_LOGI(TAG, "sensor=%s status=INIT_OK interval_ms=%d",
                 offline_sensor_name(), OFFLINE_SENSOR_LOG_INTERVAL_MS);
    }

    const BaseType_t task_ok = xTaskCreate(offline_sensor_task,
                                           "offline_sensor",
                                           OFFLINE_SENSOR_TASK_STACK_BYTES,
                                           NULL,
                                           OFFLINE_SENSOR_TASK_PRIORITY,
                                           NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "sensor=%s status=TASK_CREATE_FAIL", offline_sensor_name());
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
