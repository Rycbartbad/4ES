#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

// Convenience header: includes all ESP-NOW / Wi-Fi / netif mocks.
// Individual path-based stubs are in esp_now.h, esp_wifi.h, esp_netif.h, esp_event.h.
// Component source files include those directly; test files can use this header.
