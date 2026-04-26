#pragma once
/*
 * FreeRTOS types mock — host-side (x86) testing.
 * Component sources #include "freertos/FreeRTOS.h" → resolved here via -Itests/mocks.
 */

#include <stdint.h>

typedef int         TickType_t;
typedef void*       SemaphoreHandle_t;
typedef void*       TimerHandle_t;
typedef void*       QueueHandle_t;
typedef void*       TaskHandle_t;
typedef int         BaseType_t;

#define portMAX_DELAY       ((TickType_t)0xFFFFFFFFUL)
#define portTICK_PERIOD_MS  10
#define portTICK_RATE_MS    portTICK_PERIOD_MS

#define pdTRUE              ((BaseType_t)1)
#define pdFALSE             ((BaseType_t)0)
#define pdPASS              pdTRUE
#define pdFAIL              pdFALSE

#define pdMS_TO_TICKS(ms)   ((TickType_t)(((uint32_t)(ms) + (portTICK_PERIOD_MS) - 1) / (portTICK_PERIOD_MS)))

// Include task.h for xTaskGetTickCount and other task APIs
// ESP-IDF's FreeRTOS typically pulls this in transitively.
#include "freertos/task.h"
