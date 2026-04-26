#pragma once
/*
 * FreeRTOS task mock — host-side (x86) testing.
 * Component sources #include "freertos/task.h" → resolved here via -Itests/mocks.
 */

#include "freertos/FreeRTOS.h"

static inline BaseType_t xTaskCreate(void (*func)(void*), const char* name,
                                      uint32_t stack, void* params,
                                      unsigned priority, TaskHandle_t* handle) {
    (void)func; (void)name; (void)stack; (void)params; (void)priority;
    if (handle) *handle = (TaskHandle_t)(intptr_t)1;
    return pdPASS;
}

static inline void vTaskDelete(TaskHandle_t handle) { (void)handle; }

static inline void vTaskDelay(TickType_t ticks) { (void)ticks; }

static inline TickType_t xTaskGetTickCount(void) { return (TickType_t)0; }

static inline void vTaskSuspend(TaskHandle_t handle) { (void)handle; }

static inline void vTaskResume(TaskHandle_t handle) { (void)handle; }

static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return NULL; }

static inline void taskYIELD(void) {}
