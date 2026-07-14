/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO V1.0 — Web Console: script injection + print ring buffer.
 *
 * Implements:
 *   - Print output ring buffer (captured by builtins.cpp print())
 *   - Script abort flag + injection via script_io_enqueue()
 *
 * design.md §16.4, §16.7, §16.8
 */

#include "sdkconfig.h"

#include "web_console/script_inject.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "script_io/script_io.h"

// ====================================================================
// Log tag
// ====================================================================
static const char* TAG = "script_inject";

// ====================================================================
// Print ring buffer  — design.md §16.8
// ====================================================================

#define BUF_SIZE CONFIG_EXEC_LOG_BUF_SIZE

static char           s_print_buf[BUF_SIZE];
static int            s_write_pos = 0;   /* next write position */
static bool           s_has_wrapped = false;  /* true once write_pos wraps around */
static SemaphoreHandle_t s_print_mutex = NULL;

// ====================================================================
// Initialisation
// ====================================================================

void script_inject_init(void)
{
    if (s_print_mutex == NULL) {
        s_print_mutex = xSemaphoreCreateMutex();
        if (s_print_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create print mutex");
        }
    }
    memset(s_print_buf, 0, BUF_SIZE);
    s_write_pos = 0;
    s_has_wrapped = false;
}

// ====================================================================
// script_inject_write_print — called from builtins.cpp print()
//
// Writes into the ring buffer.  Safe to call when not initialised
// (NULL-mutex check skips the write).
// ====================================================================

void script_inject_write_print(const char* str, int len)
{
    if (s_print_mutex == NULL) return;
    if (str == NULL || len <= 0) return;

    if (xSemaphoreTake(s_print_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;  // Could not acquire lock — drop the data
    }

    for (int i = 0; i < len; i++) {
        s_print_buf[s_write_pos] = str[i];
        s_write_pos = (s_write_pos + 1) % BUF_SIZE;
        if (s_write_pos == 0) s_has_wrapped = true;
    }

    // Ensure final byte is NUL so partial reads are safe
    s_print_buf[s_write_pos] = '\0';

    xSemaphoreGive(s_print_mutex);
}

// ====================================================================
// script_inject_read_log — read entire buffer contents to caller
//
// Returns the number of bytes written into buf (not including NUL).
// The buffer is wrapped around from write_pos → end → start → write_pos.
// Returns 0 if empty.
// ====================================================================

int script_inject_read_log(char* buf, int max_len)
{
    if (s_print_mutex == NULL || buf == NULL || max_len <= 0) return 0;

    if (xSemaphoreTake(s_print_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }

    int total = 0;

    if (!s_has_wrapped) {
        // Buffer has never wrapped — data is contiguously from 0 to s_write_pos
        int to_copy = (s_write_pos < max_len) ? s_write_pos : max_len;
        if (to_copy > 0) {
            memcpy(buf, s_print_buf, (size_t)to_copy);
            total = to_copy;
        }
    } else {
        // Buffer has wrapped — data is from s_write_pos → end → 0 → s_write_pos
        int chunk1 = BUF_SIZE - s_write_pos;
        if (chunk1 > 0) {
            int to_copy = (chunk1 < max_len) ? chunk1 : max_len;
            memcpy(buf, s_print_buf + s_write_pos, (size_t)to_copy);
            total = to_copy;
        }
        if (total < max_len && s_write_pos > 0) {
            int chunk2 = s_write_pos;
            int to_copy = (chunk2 < max_len - total) ? chunk2 : (max_len - total);
            memcpy(buf + total, s_print_buf, (size_t)to_copy);
            total += to_copy;
        }
    }

    // NUL-terminate
    if (total < max_len) {
        buf[total] = '\0';
    } else {
        buf[max_len - 1] = '\0';  // safe truncation
    }

    xSemaphoreGive(s_print_mutex);
    return total;
}

// ====================================================================
// script_inject_enqueue — abort current script + enqueue new one
//
// Sequence (design.md §16.7):
//   1. Set s_script_abort_requested = true
//   2. Reset script queue (discard pending scripts)
//   3. Enqueue new script
//
// Returns 0 on success, -1 on error.
// ====================================================================

/*
 * Global abort flag — defined in app_main.cpp.
 * Web console sets it; exec_task checks it between statements.
 */
extern volatile bool s_script_abort_requested;

/*
 * Busy flag — true while exec_task is actively executing a script.
 * Defined in app_main.cpp; set/cleared around the lex→parse→execute loop.
 */
extern volatile bool s_exec_task_busy;

/*
 * Task handle for the peer timeout task — defined in app_main.cpp.
 * Suspended/resumed during Wi-Fi scanning.
 */
extern volatile TaskHandle_t s_timeout_task_handle;

int script_inject_enqueue(const char* script, int len)
{
    if (script == NULL || len <= 0) return -1;

    // Step 1: signal abort
    s_script_abort_requested = true;

    // Small delay to allow exec_task to notice the flag
    vTaskDelay(pdMS_TO_TICKS(10));

    // Step 2: flush pending scripts from queue
    script_io_reset_queue();

    // Step 3: enqueue new script
    return script_io_enqueue(script, len);
}

// ====================================================================
// Return a pointer to the timeout_task handle (for wifi_scan suspend)
// ====================================================================

volatile TaskHandle_t* script_inject_get_timeout_task_handle(void)
{
    return &s_timeout_task_handle;
}

// ====================================================================
// script_inject_is_running — true while exec_task is busy
// ====================================================================

bool script_inject_is_running(void)
{
    return s_exec_task_busy;
}
