#pragma once

/**
 * @file web_console.h
 * @brief Web Console component for ESP-LEGO — design.md §16
 *
 * Provides SoftAP hotspot, HTTP configuration page, LLM client,
 * and script injection for the ESP-LEGO master firmware.
 */

#include "sdkconfig.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the web console.
 *
 * Starts the SoftAP hotspot ("ESP-LEGO-Setup", no password) and
 * the HTTP server on 192.168.4.1.
 *
 * All configuration is read from NVS (namespace "web_console").
 * If no Wi-Fi SSID is configured, the SoftAP starts automatically
 * on first boot.
 *
 * @return ESP_OK on success, error code on failure.
 */
esp_err_t web_console_init(void);

/**
 * @brief De-initialise the web console.
 *
 * Stops the HTTP server and tears down the SoftAP.
 * Frees all resources.
 */
void web_console_deinit(void);

#ifdef __cplusplus
}
#endif
