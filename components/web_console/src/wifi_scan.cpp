/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO V1.0 — Web Console: Wi-Fi scan wrapper.
 *
 * Implements Wi-Fi scanning with ESP-NOW task suspension/resume.
 * Returns JSON array of SSID + RSSI sorted by signal strength.
 *
 * design.md §16.3, §16.11
 */

#include "sdkconfig.h"

#include "web_console/wifi_scan.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"

#include "espnow_comm/comm.h"
#include "web_console/script_inject.h"

// ====================================================================
// Log tag
// ====================================================================
static const char* TAG = "wifi_scan";

#define MAX_SCAN_APS 32

// ====================================================================
// Helper: resume suspended tasks
// ====================================================================

static void resume_tasks(TaskHandle_t timeout_task)
{
    if (timeout_task != NULL) {
        vTaskResume(timeout_task);
    }
    espnow_comm_resume_rx();
}

// ====================================================================
// wifi_scan_start — scan nearby Wi-Fi APs, return JSON
//
// Returns a cJSON object (caller must cJSON_Delete()).
// Format: [{"ssid":"...","rssi":-50}, ...]
// Sorted by RSSI descending, 2.4 GHz only.
//
// During scan: suspends ESP-NOW rx_task and timeout_task,
// resets rx_queue after scan to discard stale packets.
// ====================================================================

cJSON* wifi_scan_start(void)
{
    TaskHandle_t timeout_task = NULL;

    // ---- Suspend ESP-NOW tasks ----
    espnow_comm_suspend_rx();

    {
        volatile TaskHandle_t* h = script_inject_get_timeout_task_handle();
        timeout_task = (h != NULL) ? *h : NULL;
        if (timeout_task != NULL) {
            vTaskSuspend(timeout_task);
        }
    }

    // ---- Check Wi-Fi mode ----
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    ESP_LOGI(TAG, "starting scan, Wi-Fi mode=%d", mode);

    // ---- Configure scan (C++17-safe: memset + assign) ----
    wifi_scan_config_t scan_config;
    memset(&scan_config, 0, sizeof(scan_config));
    scan_config.scan_type            = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 100;
    scan_config.scan_time.active.max = 300;

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %d", err);
        resume_tasks(timeout_task);
        return NULL;
    }

    ESP_LOGI(TAG, "scan completed successfully");

    // ---- Get scan results ----
    wifi_ap_record_t records[MAX_SCAN_APS];
    uint16_t actual = MAX_SCAN_APS;

    err = esp_wifi_scan_get_ap_records(&actual, records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan_get_ap_records failed: %d", err);
        resume_tasks(timeout_task);
        return NULL;
    }

    ESP_LOGI(TAG, "got %d AP records", actual);

    // ---- Build JSON array ----
    cJSON* root = cJSON_CreateArray();
    if (root == NULL) {
        ESP_LOGE(TAG, "cJSON_CreateArray failed");
        resume_tasks(timeout_task);
        return NULL;
    }

    for (uint16_t i = 0; i < actual; i++) {
        const wifi_ap_record_t* ap = &records[i];

#if !CONFIG_WIFI_SCAN_SHOW_5GHZ
        if (ap->primary > 13) continue;
#endif

        if (strlen((const char*)ap->ssid) == 0) continue;

        // Dedup by SSID
        bool dup = false;
        {
            int existing = cJSON_GetArraySize(root);
            for (int j = 0; j < existing; j++) {
                cJSON* item = cJSON_GetArrayItem(root, j);
                cJSON* s = cJSON_GetObjectItem(item, "ssid");
                if (s && s->valuestring &&
                    strcmp(s->valuestring, (const char*)ap->ssid) == 0) {
                    dup = true;
                    break;
                }
            }
        }
        if (dup) continue;

        cJSON* entry = cJSON_CreateObject();
        if (entry == NULL) continue;
        cJSON_AddStringToObject(entry, "ssid", (const char*)ap->ssid);
        cJSON_AddNumberToObject(entry, "rssi", (double)ap->rssi);
        cJSON_AddItemToArray(root, entry);
    }

    // ---- Sort by RSSI descending ----
    {
        int n = cJSON_GetArraySize(root);
        for (int i = 0; i < n - 1; i++) {
            for (int j = 0; j < n - 1 - i; j++) {
                cJSON* a = cJSON_GetArrayItem(root, j);
                cJSON* b = cJSON_GetArrayItem(root, j + 1);
                if (!a || !b) continue;
                double ra = cJSON_GetObjectItem(a, "rssi")->valuedouble;
                double rb = cJSON_GetObjectItem(b, "rssi")->valuedouble;
                if (rb > ra) {
                    cJSON* tmp = cJSON_DetachItemFromArray(root, j + 1);
                    if (tmp) cJSON_InsertItemInArray(root, j, tmp);
                }
            }
        }
    }

    {
        int n = cJSON_GetArraySize(root);
        ESP_LOGI(TAG, "returning %d APs (JSON)", n);
    }

    resume_tasks(timeout_task);
    return root;
}
