#pragma once

/**
 * @file wifi_scan.h
 * @brief Wi-Fi scan wrapper — design.md §16.3
 *
 * Scans nearby Wi-Fi access points while temporarily suspending
 * ESP-NOW processing tasks.
 */

#include "sdkconfig.h"
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Perform a Wi-Fi scan and return results as a JSON array.
 *
 * The array is sorted by RSSI (strongest first). 2.4 GHz only
 * (5 GHz hidden unless CONFIG_WIFI_SCAN_SHOW_5GHZ).
 *
 * ESP-NOW rx processing is suspended during the scan and resumed
 * afterwards; the rx queue is reset to discard stale packets.
 *
 * @return cJSON array of {"ssid":"...","rssi":N}. Never returns NULL
 *         (returns empty array on error). Caller must cJSON_Delete().
 */
cJSON* wifi_scan_start(void);

#ifdef __cplusplus
}
#endif
