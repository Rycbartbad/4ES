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
// Sensor identity (from Kconfig, overrideable via sdkconfig.defaults)
//
// Default: derive module_id from the last byte of the chip's unique MAC
// address, so every sensor gets a unique ID automatically at first boot.
// Override via CONFIG_SENSOR_MODULE_ID / CONFIG_SENSOR_MODULE_NAME in
// sdkconfig.defaults.sensor or menuconfig.
// ====================================================================

#ifndef CONFIG_SENSOR_MODULE_ID
#define CONFIG_SENSOR_MODULE_ID   0  // 0 means "auto (use MAC last byte)"
#endif
#ifndef CONFIG_SENSOR_MODULE_NAME
#define CONFIG_SENSOR_MODULE_NAME ""  // empty means "auto (sensor_{id})"
#endif

#include "esp_efuse.h"
#include "esp_mac.h"

static uint8_t g_sensor_module_id = 0;
static char    g_sensor_module_name[17] = "";

static void init_sensor_identity(void) {
    // Read factory MAC address
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);

    // Determine module ID
    if (CONFIG_SENSOR_MODULE_ID != 0) {
        g_sensor_module_id = CONFIG_SENSOR_MODULE_ID;
    } else {
        // Use last byte of MAC (0-255), shifted to 1-255 range
        g_sensor_module_id = mac[5] ? mac[5] : 1;
    }

    // Determine module name
    if (CONFIG_SENSOR_MODULE_NAME[0] != '\0') {
        strncpy(g_sensor_module_name, CONFIG_SENSOR_MODULE_NAME,
                sizeof(g_sensor_module_name) - 1);
    } else {
        snprintf(g_sensor_module_name, sizeof(g_sensor_module_name),
                 "sensor_%u", g_sensor_module_id);
    }

    printf("Sensor identity: module_id=%d name=%s (MAC=%02x:%02x:%02x:%02x:%02x:%02x)\n",
           g_sensor_module_id, g_sensor_module_name,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ====================================================================
// Receive callback — handles DATA_REQ and CMD messages from master
// ====================================================================

static void sensor_recv_cb(const uint8_t* src_mac, uint8_t msg_type,
                            const uint8_t* data, int len)
{
    (void)data;
    (void)len;

    switch (msg_type) {

    case MSG_DATA_REQ: {
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
                                  g_espnow_module_id, 0,
                                  values, SENSOR_ADC_COUNT);

        if (resp_len > 0) {
            esp_now_send(src_mac, resp_buf, resp_len);
        }
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
            protocol_build_ack(ack_buf, &ack_len, g_espnow_module_id, 0);
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

    // ---- Initialize WiFi (ESP-NOW requires WiFi) ----
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // ---- Resolve sensor identity (MAC-based or Kconfig override) ----
    init_sensor_identity();

    // ---- Set module identity for ESP-NOW subsystems ----
    g_espnow_module_id   = g_sensor_module_id;
    strncpy(g_espnow_module_name, g_sensor_module_name,
            sizeof(g_espnow_module_name));
    g_espnow_module_name[sizeof(g_espnow_module_name) - 1] = '\0';

    // ---- Initialize ESP-NOW ----
    ESP_ERROR_CHECK(espnow_comm_init());

    // ---- Register receive callback ----
    espnow_comm_register_recv_callback(sensor_recv_cb);

    // ---- Create announce task ----
    BaseType_t tsk = xTaskCreate(announce_task, "announce",
                                  2048, NULL, 5, NULL);
    if (tsk != pdPASS) {
        printf("ERROR: Failed to create announce task\n");
    }

    printf("Sensor ready — module_id=%d name=%s\n",
           g_sensor_module_id, g_sensor_module_name);

    // ---- Main loop ----
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
