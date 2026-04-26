#pragma once

#include "sdkconfig.h"
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ESP-NOW communication API — design.md §7

esp_err_t espnow_comm_init(void);
void      espnow_comm_deinit(void);

// Synchronous read request (master only) — design.md §7.5
double    espnow_comm_request_read(uint8_t module_id, uint8_t pin);

// Response handler — design.md §7.7
void      espnow_comm_handle_resp(const uint8_t* src_mac, const uint8_t* data, int len);

// Send command (master → sensor)
esp_err_t espnow_comm_send_cmd(uint8_t module_id, uint16_t cmd_id, const uint8_t* payload, uint8_t payload_len);

// Send announce (sensor → broadcast)
void      espnow_comm_send_announce(void);

// Callback for received CMD/DATA_REQ (sensor mode)
typedef void (*espnow_recv_callback_t)(const uint8_t* src_mac, uint8_t msg_type,
                                       const uint8_t* data, int len);
void      espnow_comm_register_recv_callback(espnow_recv_callback_t cb);

// Module identity — set before calling espnow_comm_send_announce()
extern uint8_t g_espnow_module_id;
extern char    g_espnow_module_name[17];

// Suspend/resume RX processing (used by wifi_scan during Wi-Fi scanning)
void espnow_comm_suspend_rx(void);
void espnow_comm_resume_rx(void);

#ifdef __cplusplus
}
#endif
