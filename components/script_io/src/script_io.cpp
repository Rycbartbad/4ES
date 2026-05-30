/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO V1.0 — Script I/O via UART (P7).
 *
 * Provides a FreeRTOS queue-based line input mechanism.
 * shell_task feeds lines via fgets(stdin), exec_task consumes
 * via script_io_receive().
 *
 * Queue behaviour:
 *   - Non-blocking enqueue (timeout=0); if full, drops oldest.
 *   - Blocking dequeue (portMAX_DELAY) for exec_task.
 *   - Lines exceeding CONFIG_SCRIPT_MAX_LEN are rejected with -1.
 */

#include "sdkconfig.h"
#include "script_io/script_io.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"

// ====================================================================
// Fallback defaults (sdkconfig may not provide these yet)
// ====================================================================
#ifndef CONFIG_SCRIPT_MAX_LEN
#define CONFIG_SCRIPT_MAX_LEN 2048
#endif
#ifndef CONFIG_SCRIPT_QUEUE_LEN
#define CONFIG_SCRIPT_QUEUE_LEN 4
#endif

// ====================================================================
// Static globals
// ====================================================================
static const char* TAG = "script_io";
static QueueHandle_t s_script_queue = NULL;

// ====================================================================
// Public API
// ====================================================================

esp_err_t script_io_init(void)
{
    s_script_queue = xQueueCreate(CONFIG_SCRIPT_QUEUE_LEN,
                                  CONFIG_SCRIPT_MAX_LEN);
    if (!s_script_queue) {
        ESP_LOGE(TAG, "Failed to create script queue "
                 "(%d items, %d bytes each)",
                 CONFIG_SCRIPT_QUEUE_LEN, CONFIG_SCRIPT_MAX_LEN);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Script queue ready (%d x %d bytes)",
             CONFIG_SCRIPT_QUEUE_LEN, CONFIG_SCRIPT_MAX_LEN);
    return ESP_OK;
}

int script_io_enqueue(const char* script, int len)
{
    if (!s_script_queue) return -1;

    // Reject scripts that exceed the buffer (including NUL terminator)
    if (len >= CONFIG_SCRIPT_MAX_LEN) {
        ESP_LOGW(TAG, "Script too long (%d bytes, max %d)",
                 len, CONFIG_SCRIPT_MAX_LEN - 1);
        return -1;
    }

    char* buf = (char*)malloc(CONFIG_SCRIPT_MAX_LEN);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate script queue buffer");
        return -1;
    }
    memcpy(buf, script, (size_t)len);
    buf[len] = '\0';

    // Non-blocking send
    BaseType_t ok = xQueueSend(s_script_queue, buf, 0);
    if (ok != pdTRUE) {
        // Queue full — discard oldest item
        char* discard = (char*)malloc(CONFIG_SCRIPT_MAX_LEN);
        if (discard) {
            xQueueReceive(s_script_queue, discard, 0);
            free(discard);
        } else {
            xQueueReset(s_script_queue);
        }

        ESP_LOGW(TAG, "Script queue full, dropping oldest");

        ok = xQueueSend(s_script_queue, buf, 0);
        if (ok != pdTRUE) {
            ESP_LOGE(TAG, "Failed to enqueue even after dropping oldest");
            free(buf);
            return -1;
        }
    }

    free(buf);
    return 0;
}

void script_io_reset_queue(void)
{
    if (s_script_queue) {
        xQueueReset(s_script_queue);
    }
}

int script_io_receive(char* buf, int max_len, TickType_t timeout)
{
    if (!s_script_queue || !buf || max_len < CONFIG_SCRIPT_MAX_LEN) {
        return -1;
    }

    if (xQueueReceive(s_script_queue, buf, timeout) == pdTRUE) {
        return (int)strlen(buf);
    }

    return 0;  // timeout, nothing received
}
