#pragma once

#include "sdkconfig.h"
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "espnow_comm/protocol.h"

// Public API — mirrors espnow_comm/comm.h for drop-in replacement

esp_err_t tcp_comm_init(void);
void      tcp_comm_deinit(void);

// Synchronous read request (master only) — same signature as espnow_comm_request_read
int       tcp_comm_request_read(uint8_t module_id, double* out_values, int max_values);

// Send command (master → sensor)
esp_err_t tcp_comm_send_cmd(uint8_t module_id, uint16_t cmd_id,
                             const uint8_t* payload, uint16_t payload_len);

// Send raw data on current connection (sensor mode)
int       tcp_comm_send_raw(const uint8_t* data, int len);

// Callback for received CMD/DATA_REQ (sensor mode) — same signature
typedef void (*tcp_recv_callback_t)(const uint8_t* src_mac, uint8_t msg_type,
                                     const uint8_t* data, int len);
void      tcp_comm_register_recv_callback(tcp_recv_callback_t cb);

// Module name + capability (set by sensor before init)
extern char    g_tcp_module_name[17];
extern char    g_tcp_module_capability[CONFIG_MAX_CAPABILITY_LEN];

#ifdef __cplusplus
}
#endif
