#pragma once

/**
 * @file script_io.h
 * @brief Script input via UART — design.md §16.3
 *
 * Provides a FreeRTOS queue-based line input mechanism for
 * feeding scripts into the interpreter's exec_task.
 */

#include "sdkconfig.h"
#include <stdint.h>

#include "freertos/FreeRTOS.h"   /* TickType_t */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the script I/O subsystem.
 *
 * Creates the internal FreeRTOS queue of size
 * CONFIG_SCRIPT_QUEUE_LEN × CONFIG_SCRIPT_MAX_LEN bytes.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM on queue creation failure.
 */
esp_err_t script_io_init(void);

/**
 * @brief Non-blocking enqueue of a script line.
 *
 * Copies the script (up to len bytes) into the queue.
 * If the queue is full the oldest item is silently dropped.
 *
 * @param script Pointer to script data (does not need to be NUL-terminated).
 * @param len    Number of bytes to enqueue.
 * @return 0 on success, -1 if script exceeds CONFIG_SCRIPT_MAX_LEN.
 */
int       script_io_enqueue(const char* script, int len);

/**
 * @brief Reset the script queue, discarding all pending scripts.
 */
void script_io_reset_queue(void);

/**
 * @brief Blocking dequeue of a script line.
 *
 * Blocks up to `timeout` ticks for a script to become available.
 * The received line is NUL-terminated.
 *
 * @param buf     Destination buffer (must be >= CONFIG_SCRIPT_MAX_LEN).
 * @param max_len Size of destination buffer.
 * @param timeout Max ticks to wait (use portMAX_DELAY for infinite).
 * @return Number of bytes received (excluding NUL), 0 on timeout,
 *         -1 on error (queue not initialised or buffer too small).
 */
int       script_io_receive(char* buf, int max_len, TickType_t timeout);

#ifdef __cplusplus
}
#endif
