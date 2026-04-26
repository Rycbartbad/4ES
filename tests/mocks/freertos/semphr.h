#pragma once
/*
 * FreeRTOS semaphore mock — host-side (x86) testing.
 * Component sources #include "freertos/semphr.h" → resolved here via -Itests/mocks.
 */

#include "freertos/FreeRTOS.h"

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return (SemaphoreHandle_t)(intptr_t)1;
}

static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    return (SemaphoreHandle_t)(intptr_t)1;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) {
    (void)s; (void)t;
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    (void)s;
    return pdTRUE;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t s) {
    (void)s;
}
