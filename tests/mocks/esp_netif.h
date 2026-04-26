#pragma once
/*
 * ESP-NETIF mock — host-side (x86) testing.
 * Component sources #include "esp_netif.h" → resolved here via -Itests/mocks.
 */

#include "esp_err.h"

static inline esp_err_t esp_netif_init(void) { return ESP_OK; }
