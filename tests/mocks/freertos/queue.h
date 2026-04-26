#pragma once
/*
 * FreeRTOS queue mock — host-side (x86) testing.
 * Component sources #include "freertos/queue.h" → resolved here via -Itests/mocks.
 */

#include "freertos/FreeRTOS.h"

static inline QueueHandle_t xQueueCreate(int depth, int item_size) {
    (void)depth; (void)item_size;
    return (QueueHandle_t)(intptr_t)1;
}

static inline BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t t) {
    (void)q; (void)item; (void)t;
    return pdTRUE;
}

static inline BaseType_t xQueueReceive(QueueHandle_t q, void* buf, TickType_t t) {
    (void)q; (void)buf; (void)t;
    return pdTRUE;
}

static inline BaseType_t xQueueSendFromISR(QueueHandle_t q, const void* item,
                                            BaseType_t* higher_prio_woken) {
    (void)q; (void)item; (void)higher_prio_woken;
    return pdTRUE;
}

static inline void xQueueReset(QueueHandle_t q) { (void)q; }

static inline void vQueueDelete(QueueHandle_t q) { (void)q; }
