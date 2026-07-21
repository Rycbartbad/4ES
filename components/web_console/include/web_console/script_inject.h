#pragma once

/**
 * @file script_inject.h
 * @brief Script injection and print ring buffer API — design.md §16.4, §16.7
 */

#include "sdkconfig.h"
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the script inject subsystem.
 *
 * Creates the print ring buffer mutex and zeroes the buffer.
 */
void script_inject_init(void);

/**
 * @brief Write data into the print ring buffer.
 *
 * Called from builtins.cpp print() to capture script output.
 * Safe to call before script_inject_init() (no-op if not initialised).
 *
 * @param str String data to write.
 * @param len Number of bytes to write.
 */
void script_inject_write_print(const char* str, int len);

/**
 * @brief Read the print ring buffer contents.
 *
 * Copies the entire buffer contents into buf and NUL-terminates it.
 *
 * @param buf     Destination buffer.
 * @param max_len Maximum bytes to read (must leave room for NUL).
 * @return Number of bytes written (excluding NUL), 0 if empty.
 */
int  script_inject_read_log(char* buf, int max_len);

/**
 * @brief Abort current script and enqueue a new one.
 *
 * Sets s_script_abort_requested, flushes any pending scripts from
 * the queue, then enqueues the new script.
 *
 * @param script Script source code.
 * @param len    Script length in bytes.
 * @return 0 on success, -1 on error.
 */
int  script_inject_enqueue(const char* script, int len);

/**
 * @brief Get the timeout task handle pointer (for suspend during scan).
 *
 * @return Pointer to the volatile TaskHandle_t for the timeout task.
 */
volatile TaskHandle_t* script_inject_get_timeout_task_handle(void);

/**
 * @brief Query whether exec_task is currently executing a script.
 *
 * Returns true from the moment exec_task starts lexing/parsing/executing
 * until it finishes cleanup and returns to the dequeue-wait state.
 * Safe to call from any task; backed by s_exec_task_busy in app_main.cpp.
 *
 * @return true if a script is actively running.
 */
bool script_inject_is_running(void);

#ifdef __cplusplus
}
#endif
