#pragma once
/*
 * ESP-Wi-Fi mock — host-side (x86) testing.
 * Component sources #include "esp_wifi.h" → resolved here via -Itests/mocks.
 */

#include <stdint.h>
#include "esp_err.h"

// Wi-Fi types
typedef struct { uint8_t _dummy; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})

#define WIFI_MODE_STA           1
#define WIFI_MODE_AP            2
#define WIFI_MODE_APSTA         3
#define WIFI_IF_STA             0
#define WIFI_IF_AP              1
#define WIFI_SECOND_CHAN_NONE   0

// Stub functions
static inline esp_err_t esp_wifi_init(const wifi_init_config_t* cfg) {
    (void)cfg; return ESP_OK;
}

static inline esp_err_t esp_wifi_set_mode(int mode) {
    (void)mode; return ESP_OK;
}

static inline esp_err_t esp_wifi_start(void) { return ESP_OK; }

static inline esp_err_t esp_wifi_stop(void) { return ESP_OK; }

static inline esp_err_t esp_wifi_set_channel(uint8_t channel, uint8_t second_chan) {
    (void)channel; (void)second_chan; return ESP_OK;
}
