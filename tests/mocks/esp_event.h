#pragma once
/*
 * ESP event loop mock — host-side (x86) testing.
 * Component sources #include "esp_event.h" → resolved here via -Itests/mocks.
 */

#include "esp_err.h"

static inline esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
