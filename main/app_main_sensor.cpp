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
// Sensor identity (configurable at build time or from NVS)
// ====================================================================

#define SENSOR_MODULE_ID   1
#define SENSOR_MODULE_NAME "sensor_1"

// ====================================================================
// Receive callback — handles DATA_REQ and CMD messages from master
// ====================================================================

static void sensor_recv_cb(const uint8_t* src_mac, uint8_t msg_type,
                            const uint8_t* data, int len)
{
    switch (msg_type) {

    case MSG_DATA_REQ: {
        // DATA_REQ payload: 1 byte (pin number)
        // Note: data points to payload AFTER MsgHeader, so pin = data[0]
        if (len < 1) break;

        uint8_t pin = data[0];

        // Read the sensor value (ADC for analog sensors)
        int raw = hw_adc_read(pin);

        // Build DATA_RESP packet (full message including header)
        uint8_t resp_buf[64];
        size_t  resp_len = 0;

        // seq_id=0 is used since the callback does not pass the header.
        // The master's comm component still matches the response via the
        // semaphore-based mechanism (design.md §7.5).
        protocol_build_data_resp(resp_buf, &resp_len,
                                  g_espnow_module_id, 0,
                                  (double)raw);

        if (resp_len > 0) {
            esp_now_send(src_mac, resp_buf, resp_len);
        }

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

    // ---- Set module identity ----
    g_espnow_module_id   = SENSOR_MODULE_ID;
    strncpy(g_espnow_module_name, SENSOR_MODULE_NAME,
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
           SENSOR_MODULE_ID, SENSOR_MODULE_NAME);

    // ---- Main loop ----
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
