/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO V1.0 — Master firmware entry point (P7).
 *
 * Device role: MASTER (CONFIG_DEVICE_ROLE_MASTER).
 *
 * Task architecture:
 *   shell_task   — reads lines from UART stdin, enqueues scripts
 *   timeout_task — periodic peer aging scan (1s interval)
 *   exec_task    — dequeues scripts, lex → parse → execute → cleanup
 *
 * ESP-NOW RX processing runs internally in comm.cpp's own rx_task.
 */

#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "interpreter/intern.h"
#include "interpreter/ast.h"
#include "interpreter/lexer.h"
#include "interpreter/parser.h"
#include "interpreter/environment.h"
#include "interpreter/interpreter.h"
#include "interpreter/builtins.h"

#include "hw_drivers/drivers.h"

#include "lcd_touch/lcd_touch.h"
#include "ui_lvgl/ui_lvgl.h"

#include "espnow_comm/peer_mgr.h"
#include "espnow_comm/comm.h"

#include "script_io/script_io.h"

#if CONFIG_WEB_CONSOLE_ENABLED
#include "web_console/web_console.h"
#endif

// ====================================================================
// Fallback defaults (when sdkconfig not yet configured)
// ====================================================================
#ifndef CONFIG_SCRIPT_MAX_LEN
#define CONFIG_SCRIPT_MAX_LEN 2048
#endif

// ====================================================================
// Constants
// ====================================================================
static const char* TAG = "esp_lego";

// ====================================================================
// Global interpreter state (persists across scripts)
// ====================================================================
static Environment s_global_env;
static ExecutionContext s_ctx;

// ====================================================================
// Global control flags
//
// s_script_timeout  — set by watchdog timer (P8), checked by interpreter
// s_script_abort    — set by web_console (P7.5) to request early termination
// ====================================================================
volatile bool s_script_timeout = false;
volatile bool s_script_abort_requested = false;

// Task handle for timeout_task — referenced by web_console wifi_scan
// (for suspend/resume during scan).  Defined here, declared extern in
// script_inject.cpp.
volatile TaskHandle_t s_timeout_task_handle = NULL;

// ====================================================================
// Watchdog callback — set timeout flag from timer task context.
// Timer task context: NO locks, NO blocking calls, NO printf.
// Memory barrier ensures the write is visible cross-core (P8).
// ====================================================================

static void IRAM_ATTR watchdog_cb(TimerHandle_t xTimer) {
    (void)xTimer;
    s_script_timeout = true;
    __sync_synchronize();
}

// ====================================================================
// Track output pins for hardware safety reset on script end.
// Called by builtins (digital_write, analog_write, send_motor).
// ====================================================================

static uint8_t s_used_pins[16];
static int     s_used_pin_count = 0;

void track_output_pin(uint8_t pin) {
    for (int i = 0; i < s_used_pin_count; i++) {
        if (s_used_pins[i] == pin) return;
    }
    if (s_used_pin_count < 16) {
        s_used_pins[s_used_pin_count++] = pin;
    }
}

// ====================================================================
// on_script_end — hardware safety: reset all tracked output pins.
// Called after every script execution (normal, abort, or error).
// ====================================================================

static void on_script_end(void) {
    for (int i = 0; i < s_used_pin_count; i++) {
        hw_gpio_write(s_used_pins[i], 0);   // reset to safe state
    }
    s_used_pin_count = 0;
}

// ====================================================================
// Forward declarations of task functions
// ====================================================================
static void shell_task(void* pv);
static void timeout_task(void* pv);
static void exec_task(void* pv);

// ====================================================================
// app_main — master firmware entry point
// ====================================================================

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "ESP-LEGO V1.0 Master starting...");

    // ---- 1. Initialise NVS (required by ESP-NOW) ----
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS and retrying...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ---- 2. Initialise ESP-NOW (Wi-Fi + peer_mgr + rx_task) ----
    ESP_ERROR_CHECK(espnow_comm_init());

    // ---- 2b. Initialise LCD (ST7789, 240×240) + Touch (CST816D) ----
    bool lcd_ready = false;
    {
        esp_err_t lcd_ret = lcd_touch_init();
        if (lcd_ret != ESP_OK) {
            ESP_LOGW(TAG, "LCD/Touch init skipped (%s), continuing",
                     esp_err_to_name(lcd_ret));
        } else {
            ESP_LOGI(TAG, "LCD + Touch ready");
            lcd_ready = true;
        }
    }

    // ---- 2c. Initialise onboard LVGL diagnostics UI (non-fatal) ----
    if (lcd_ready) {
        esp_err_t ui_ret = ui_lvgl_init();
        if (ui_ret != ESP_OK) {
            ESP_LOGW(TAG, "LVGL UI init skipped (%s), continuing",
                     esp_err_to_name(ui_ret));
        } else {
            ESP_LOGI(TAG, "LVGL diagnostics UI ready");
        }
    }

    // ---- 3. Initialise interpreter ----
    ast_pool_init();
    env_init(&s_global_env, NULL);
    register_builtins(&s_global_env);
    env_snapshot(&s_global_env);   // save pristine state (builtins only)
    ctx_init(&s_ctx);

    ESP_LOGI(TAG, "Interpreter ready (%d builtins registered)",
             BIF_COUNT);

    // ---- 4. Initialise script I/O queue ----
    ESP_ERROR_CHECK(script_io_init());

    // ---- 5. Create FreeRTOS tasks ----
    BaseType_t t;

    t = xTaskCreate(shell_task, "shell", 4096, NULL, 4, NULL);
    if (t != pdPASS) {
        ESP_LOGE(TAG, "Failed to create shell_task");
    }

    t = xTaskCreate(timeout_task, "timeout", 2048, NULL, 3,
                     (TaskHandle_t*)&s_timeout_task_handle);
    if (t != pdPASS) {
        ESP_LOGE(TAG, "Failed to create timeout_task");
    }

    t = xTaskCreate(exec_task, "exec", 8192, NULL, 5, NULL);
    if (t != pdPASS) {
        ESP_LOGE(TAG, "Failed to create exec_task");
    }

    // ---- 6. Initialise web console (SoftAP + HTTP server) ----
#if CONFIG_WEB_CONSOLE_ENABLED
    web_console_init();
#endif

    ESP_LOGI(TAG, "All tasks created. Ready. Send a script via UART.");

    // app_main returns — FreeRTOS scheduler keeps tasks running
}

// ====================================================================
// shell_task — reads lines from UART stdin, enqueues as scripts
//
// Uses fgets() which is line-buffered via the VFS UART driver.
// Backspace/line-editing is provided by the terminal emulator.
// ====================================================================

static void shell_task(void* pv)
{
    (void)pv;
    char line[CONFIG_SCRIPT_MAX_LEN];

    while (1) {
        if (fgets(line, sizeof(line), stdin)) {
            // Strip trailing newline/carriage-return
            int len = (int)strlen(line);
            while (len > 0 &&
                   (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }

            if (len > 0) {
                ESP_LOGD(TAG, "RX: %s", line);
                script_io_enqueue(line, len);
            }
        } else {
            // fgets returned NULL (EOF or error) — yield briefly
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// ====================================================================
// timeout_task — periodic peer aging scan
//
// Scans the peer table and transitions stale entries to OFFLINE.
// Runs every CONFIG_PEER_TIMEOUT_MS / 2 ≈ every 1 s (hard-coded).
// ====================================================================

static void timeout_task(void* pv)
{
    (void)pv;
    while (1) {
        peer_mgr_age_scan(xTaskGetTickCount());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ====================================================================
// exec_task — dequeues scripts, parses, executes, cleans up
//
// Each script runs in a clean environment:
//   1. Wait for a script via script_io_receive()
//   2. Reset all pools (AST, intern, execution context)
//   3. Restore global environment to pristine state (builtins only)
//   4. Point timeout check at the global flag
//   5. Lex → Parse → Validate → Execute
//   6. Log errors if any
//   7. Clean up for next script
//
// Stack: 8192 bytes (design.md §5.1: recursive parser + interpreter)
// ====================================================================

static void exec_task(void* pv)
{
    (void)pv;
    char script[CONFIG_SCRIPT_MAX_LEN];
    Lexer lexer;
    Parser parser;
    TimerHandle_t wdt = NULL;

    while (1) {
        // ---- 1. Wait for a script ----
        int len = script_io_receive(script, sizeof(script), portMAX_DELAY);
        if (len <= 0) {
            continue;
        }

        ESP_LOGI(TAG, "Execute (%d bytes)", len);

        // ---- 2. Reset for a fresh script ----
        ast_pool_reset();
        intern_reset();
        env_restore_pristine(&s_global_env);
        ctx_reset(&s_ctx);

        // ---- 3. Start watchdog timer ----
        // Reset must be visible cross-core before watchdog can fire.
        s_script_timeout        = false;
        s_script_abort_requested = false;
        __sync_synchronize();

        wdt = xTimerCreate("wd",
                           pdMS_TO_TICKS(CONFIG_SCRIPT_EXEC_TIMEOUT_MS),
                           pdFALSE, NULL, watchdog_cb);
        if (wdt) {
            s_ctx.s_script_timeout_ptr = &s_script_timeout;
            xTimerStart(wdt, portMAX_DELAY);
        } else {
            s_ctx.s_script_timeout_ptr = NULL;
            ESP_LOGW(TAG, "Failed to create watchdog timer");
        }

        // ---- 4. Parse ----
        lexer_init(&lexer, script);
        parser_init(&parser, &lexer);
        ASTNode* ast = parser_parse(&parser);

        if (!parser.had_error && ast) {
            // ---- 5. Validate resources ----
            ResourceReport rpt = validate_resources(ast, &s_global_env);
            if (rpt.passed) {
                // ---- 6. Execute ----
                execute(ast, &s_global_env, &s_ctx);
            } else {
                ESP_LOGE(TAG, "Resource validation failed: %s",
                         rpt.fail_reason ? rpt.fail_reason : "unknown");
                // Skip execution — resources exceed safe limits
            }
        }

        // ---- 7. Log errors ----
        if (parser.had_error) {
            ESP_LOGE(TAG, "Parse error L%d:%d %s",
                     parser.error_line, parser.error_col,
                     parser.error_msg ? parser.error_msg : "unknown");
        } else if (s_ctx.constraint_violated) {
            ESP_LOGE(TAG, "Runtime error: %s",
                     s_ctx.violation_msg
                         ? s_ctx.violation_msg
                         : "constraint violated");
        }
        if (s_script_timeout) {
            ESP_LOGE(TAG, "Script aborted: execution timeout");
        }
        if (s_script_abort_requested) {
            ESP_LOGW(TAG, "Script aborted by user request");
        }

        // ---- 8. Hardware safety — reset tracked output pins ----
        on_script_end();

        // ---- 9. Stop and delete watchdog ----
        if (wdt) {
            xTimerStop(wdt, portMAX_DELAY);
            xTimerDelete(wdt, portMAX_DELAY);
            wdt = NULL;
        }
        s_ctx.s_script_timeout_ptr = NULL;
        s_script_timeout           = false;
        __sync_synchronize();

        // ---- 10. Final cleanup before next script ----
        ast_pool_reset();
        ctx_reset(&s_ctx);
        s_script_abort_requested = false;

        ESP_LOGI(TAG, "Script done - ready for next");
    }
}
