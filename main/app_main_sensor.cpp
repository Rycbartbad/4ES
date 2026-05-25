/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO V1.0 — Sensor firmware entry point
 *
 * SENSOR device role (CONFIG_DEVICE_ROLE_SENSOR):
 *   - Announced via ESP-NOW broadcast
 *   - Responds to DATA_REQ by reading ADC/GPIO
 *   - Executes CMD messages (e.g. digital_write)
 *
 * Prerequisites for building:
 *   idf.py menuconfig → ESP-LEGO Device Role → Sensor
 */

#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "espnow_comm/comm.h"
#include "espnow_comm/protocol.h"

#include "hw_drivers/drivers.h"

// ====================================================================
// Configuration defaults (when sdkconfig.h not yet fully configured)
// ====================================================================

#ifndef CONFIG_ANNOUNCE_INTERVAL_MS
#define CONFIG_ANNOUNCE_INTERVAL_MS 3000
#endif

// ====================================================================
// Module name (from Kconfig)
// ====================================================================

#ifndef CONFIG_SENSOR_MODULE_NAME
#define CONFIG_SENSOR_MODULE_NAME "sensor"
#endif

static const char* s_module_name = CONFIG_SENSOR_MODULE_NAME;

// ====================================================================
// Active Sensor Configuration (Set one to 1, others to 0)
// ====================================================================
#define USE_SENSOR_DHT11     0
#define USE_SENSOR_VIBRATION 0
#define USE_SENSOR_RAINDROP  0
#define USE_SENSOR_BH1750    0
#define USE_SENSOR_JW01      1

// ====================================================================
// DHT11 Sensor Definition
// ====================================================================
#if USE_SENSOR_DHT11
#include "driver/gpio.h"
#include "esp_rom_sys.h"

#define DHT11_PIN GPIO_NUM_13

static bool dht11_read(double* temperature, double* humidity) {
    uint8_t data[5] = {0, 0, 0, 0, 0};

    // Send start signal
    gpio_set_direction(DHT11_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT11_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20)); // DHT11 requires min 18ms
    gpio_set_level(DHT11_PIN, 1);
    esp_rom_delay_us(30);

    // Prepare to read
    gpio_set_direction(DHT11_PIN, GPIO_MODE_INPUT);

    // Wait for DHT11 response signal (low then high)
    int wait_time = 0;
    while (gpio_get_level(DHT11_PIN) == 1 && wait_time < 100) { esp_rom_delay_us(1); wait_time++; }
    if (wait_time >= 100) return false;
    wait_time = 0;
    while (gpio_get_level(DHT11_PIN) == 0 && wait_time < 100) { esp_rom_delay_us(1); wait_time++; }
    if (wait_time >= 100) return false;
    wait_time = 0;
    while (gpio_get_level(DHT11_PIN) == 1 && wait_time < 100) { esp_rom_delay_us(1); wait_time++; }
    if (wait_time >= 100) return false;

    // Read 40 bits of data
    for (int i = 0; i < 40; i++) {
        wait_time = 0;
        while (gpio_get_level(DHT11_PIN) == 0 && wait_time < 100) { esp_rom_delay_us(1); wait_time++; }
        
        wait_time = 0;
        while (gpio_get_level(DHT11_PIN) == 1 && wait_time < 100) { esp_rom_delay_us(1); wait_time++; }
        
        data[i / 8] <<= 1;
        if (wait_time > 40) {
            data[i / 8] |= 1;
        }
    }

    uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (checksum == data[4]) {
        *humidity = (double)data[0] + ((double)data[1] / 10.0);
        *temperature = (double)data[2] + ((double)data[3] / 10.0);
        return true;
    }
    return false;
}
#endif

// ====================================================================
// Vibration Sensor Definition
// ====================================================================
#if USE_SENSOR_VIBRATION
#include "driver/gpio.h"

#define VIBRATION_PIN GPIO_NUM_6

static void vibration_init() {
    gpio_set_direction(VIBRATION_PIN, GPIO_MODE_INPUT);
    // 使用下拉电阻确保无震动时为低电平（如果传感器没有内置下拉的话）
    gpio_set_pull_mode(VIBRATION_PIN, GPIO_PULLDOWN_ONLY);
}

static double vibration_read() {
    // 读取电平，有震动时(高电平)返回1.0，无震动时返回0.0
    return (double)gpio_get_level(VIBRATION_PIN);
}
#endif

// ====================================================================
// Raindrop Sensor Definition
// ====================================================================
#if USE_SENSOR_RAINDROP
#include "driver/gpio.h"

#define RAINDROP_PIN GPIO_NUM_6

static void raindrop_init() {
    gpio_set_direction(RAINDROP_PIN, GPIO_MODE_INPUT);
    // 使用下拉电阻确保无雨滴时为低电平（防止引脚悬空）
    gpio_set_pull_mode(RAINDROP_PIN, GPIO_PULLDOWN_ONLY);
}

static double raindrop_read() {
    // 读取电平，有雨滴时(高电平)返回1.0，无雨滴时返回0.0
    return (double)gpio_get_level(RAINDROP_PIN);
}
#endif

// ====================================================================
// BH1750 Light Sensor Definition
// ====================================================================
#if USE_SENSOR_BH1750
#include "driver/i2c.h"

#define I2C_MASTER_SCL_IO           GPIO_NUM_22 // SCL
#define I2C_MASTER_SDA_IO           GPIO_NUM_21 // SDA
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000
#define BH1750_SENSOR_ADDR          0x23
#define BH1750_CMD_START            0x23 // One-time High Res mode (usually 0x20 or 0x23, 0x20 for High Res Mode 1)

static void bh1750_init() {
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_MASTER_SDA_IO;
    conf.scl_io_num = I2C_MASTER_SCL_IO;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_MASTER_FREQ_HZ;
    conf.clk_flags = 0;
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

static double bh1750_read() {
    uint8_t data[2] = {0, 0};
    
    // Send measurement command (One time H-resolution mode 0x20)
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BH1750_SENSOR_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0x20, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    // Wait for measurement to complete (High-res mode max 180ms)
    vTaskDelay(pdMS_TO_TICKS(180));

    // Read 2 bytes of data
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BH1750_SENSOR_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &data[0], I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &data[1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    double lux = ((data[0] << 8) | data[1]) / 1.2;
    return lux;
}
#endif

// ====================================================================
// JW01 Gas Sensor Definition
// ====================================================================
#if USE_SENSOR_JW01
#include "driver/uart.h"
#include "driver/gpio.h"

#define JW01_UART_NUM      UART_NUM_1
#define JW01_TX_PIN        GPIO_NUM_16 // ESP32-S3 TX -> JW01 RX
#define JW01_RX_PIN        GPIO_NUM_15 // ESP32-S3 RX -> JW01 TX
#define JW01_BAUD_RATE     9600

static void jw01_init() {
    uart_config_t uart_config = {};
    uart_config.baud_rate = JW01_BAUD_RATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity    = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    
    uart_param_config(JW01_UART_NUM, &uart_config);
    uart_set_pin(JW01_UART_NUM, JW01_TX_PIN, JW01_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(JW01_UART_NUM, 256, 0, 0, NULL, 0); 
}

static bool jw01_read(double* co2, double* tvoc, double* ch2o) {
    uint8_t data[64];
    // Read whatever is in the RX buffer (Wait up to 100ms)
    int length = uart_read_bytes(JW01_UART_NUM, data, sizeof(data), pdMS_TO_TICKS(100));
    
    if (length > 0) {
        // JW01 typically sends frames of data continuously or upon request.
        // As a generic implementation for 3-in-1 gas sensor (CO2, TVOC, CH2O):
        // Assuming byte indices for the gas properties (e.g., standard 9-byte packet).
        // A typical module might send: [StartByte] [Gas1_H] [Gas1_L] [Gas2_H] [Gas2_L] ... 
        // Here we extract basic raw values assuming data length is sufficient.
        // You might need to adjust indices according to JW01's exact datasheet.
        if (length >= 9) {
            *co2  = (double)((data[1] << 8) | data[2]); // Mock CO2 index
            *tvoc = (double)((data[3] << 8) | data[4]); // Mock TVOC index
            *ch2o = (double)((data[5] << 8) | data[6]); // Mock CH2O index
            return true;
        }
    }
    return false;
}
#endif

// ====================================================================
// Receive callback — handles DATA_REQ and CMD messages from master
// ====================================================================

static void sensor_recv_cb(const uint8_t* src_mac, uint8_t msg_type,
                            const uint8_t* data, int len)
{
    (void)data;
    (void)len;

    // Ensure peer is added to ESP-NOW so unicast esp_now_send succeeds
    if (!esp_now_is_peer_exist(src_mac)) {
        esp_now_peer_info_t peerInfo = {};
        peerInfo.channel = 0; // Use current channel
        peerInfo.encrypt = false;
        memcpy(peerInfo.peer_addr, src_mac, 6);
        esp_now_add_peer(&peerInfo);
    }

    switch (msg_type) {

    case MSG_DATA_REQ: {
#if USE_SENSOR_JW01
        static bool init_done = false;
        if (!init_done) {
            jw01_init();
            init_done = true;
        }

        double values[3] = {0.0, 0.0, 0.0};
        double co2 = 0.0, tvoc = 0.0, ch2o = 0.0;
        
        if (jw01_read(&co2, &tvoc, &ch2o)) {
            values[0] = co2;
            values[1] = tvoc;
            values[2] = ch2o;
        }

        // Build DATA_RESP with three gas data values (CO2, TVOC, CH2O)
        uint8_t resp_buf[128];
        size_t  resp_len = 0;
        protocol_build_data_resp(resp_buf, &resp_len,
                                  0, 0,
                                  values, 3);

        if (resp_len > 0) {
            esp_now_send(src_mac, resp_buf, resp_len);
        }
#elif USE_SENSOR_BH1750
        static bool init_done = false;
        if (!init_done) {
            bh1750_init();
            init_done = true;
        }

        double values[1] = {0.0};
        values[0] = bh1750_read();

        // Build DATA_RESP with luminosity (lux)
        uint8_t resp_buf[128];
        size_t  resp_len = 0;
        protocol_build_data_resp(resp_buf, &resp_len,
                                  0, 0,
                                  values, 1);

        if (resp_len > 0) {
            esp_now_send(src_mac, resp_buf, resp_len);
        }
#elif USE_SENSOR_RAINDROP
        static bool init_done = false;
        if (!init_done) {
            raindrop_init();
            init_done = true;
        }

        double values[1] = {0.0};
        values[0] = raindrop_read();

        // Build DATA_RESP with raindrop status (1.0 = raindrop, 0.0 = no raindrop)
        uint8_t resp_buf[128];
        size_t  resp_len = 0;
        protocol_build_data_resp(resp_buf, &resp_len,
                                  0, 0,
                                  values, 1);

        if (resp_len > 0) {
            esp_now_send(src_mac, resp_buf, resp_len);
        }
#elif USE_SENSOR_VIBRATION
        static bool init_done = false;
        if (!init_done) {
            vibration_init();
            init_done = true;
        }

        double values[1] = {0.0};
        values[0] = vibration_read();

        // Build DATA_RESP with vibration status (1.0 = vibration, 0.0 = no vibration)
        uint8_t resp_buf[128];
        size_t  resp_len = 0;
        protocol_build_data_resp(resp_buf, &resp_len,
                                  0, 0,
                                  values, 1);

        if (resp_len > 0) {
            esp_now_send(src_mac, resp_buf, resp_len);
        }
#elif USE_SENSOR_DHT11
        double values[2] = {0.0, 0.0};
        double temp = 0.0, hum = 0.0;
        
        if (dht11_read(&temp, &hum)) {
            values[0] = temp;
            values[1] = hum;
        }

        // Build DATA_RESP with temperature and humidity
        uint8_t resp_buf[128];
        size_t  resp_len = 0;
        protocol_build_data_resp(resp_buf, &resp_len,
                                  0, 0,
                                  values, 2);

        if (resp_len > 0) {
            esp_now_send(src_mac, resp_buf, resp_len);
        }
#else
        // Read ALL local sensor pins and pack them into a structured response.
        // Submodule firmware defines which pins to read; the master receives
        // all values and returns them to the script as a list or single number.
        //
        // Configure SENSOR_ADC_PINS to match your hardware layout.
        #define SENSOR_ADC_PINS    {4, 5, 6}       // GPIO pins to read (ADC)
        #define SENSOR_ADC_COUNT  3

        static const uint8_t s_adc_pins[SENSOR_ADC_COUNT] = SENSOR_ADC_PINS;
        double values[SENSOR_ADC_COUNT];

        for (int i = 0; i < SENSOR_ADC_COUNT; i++) {
            values[i] = (double)hw_adc_read(s_adc_pins[i]);
        }

        // Build DATA_RESP with all values
        uint8_t resp_buf[128];
        size_t  resp_len = 0;
        protocol_build_data_resp(resp_buf, &resp_len,
                                  0, 0,
                                  values, SENSOR_ADC_COUNT);

        if (resp_len > 0) {
            esp_now_send(src_mac, resp_buf, resp_len);
        }
#endif
        break;
    }

    case MSG_CMD: {
        // Simple command handling: payload[0] = pin, payload[1] = value
        if (len < 2) break;
        uint8_t pin = data[0];
        uint8_t val = data[1];
        hw_gpio_write(pin, val);

        // Send ACK
        {
            uint8_t ack_buf[64];
            size_t ack_len = 0;
            protocol_build_ack(ack_buf, &ack_len, 0, 0);
            if (ack_len > 0) {
                esp_now_send(src_mac, ack_buf, ack_len);
            }
        }
        break;
    }

    default:
        break;
    }
}

// ====================================================================
// Announce task — sends periodic broadcast announces
// ====================================================================

static void announce_task(void* arg)
{
    (void)arg;
    while (1) {
        espnow_comm_send_announce();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_ANNOUNCE_INTERVAL_MS));
    }
}

// ====================================================================
// app_main — sensor entry point
// ====================================================================

extern "C" void app_main(void)
{
    printf("ESP-LEGO V1.0 SENSOR firmware starting...\n");

    // ---- Initialize NVS ----
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ---- Initialize WiFi and ESP-NOW ----
    // espnow_comm_init() safely handles all wifi netif, event loops, and wifi_init setup
    ESP_ERROR_CHECK(espnow_comm_init());

    // ---- Set module name (used in announce) ----
    strncpy(g_espnow_module_name, s_module_name,
            sizeof(g_espnow_module_name));
    g_espnow_module_name[sizeof(g_espnow_module_name) - 1] = '\0';

    // ---- Register receive callback ----
    espnow_comm_register_recv_callback(sensor_recv_cb);

    // ---- Create announce task ----
    BaseType_t tsk = xTaskCreate(announce_task, "announce",
                                  2048, NULL, 5, NULL);
    if (tsk != pdPASS) {
        printf("ERROR: Failed to create announce task\n");
    }

    printf("Sensor ready — name=%s\n",
           g_espnow_module_name);

    // ---- Main loop ----
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
