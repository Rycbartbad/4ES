#pragma once

#include "sdkconfig.h"
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "protocol.h"

// ESP-NOW communication API — design.md §7

esp_err_t espnow_comm_init(void);
void      espnow_comm_deinit(void);

// Synchronous read request (master only) — design.md §7.5
// Sends DATA_REQ, waits for DATA_RESP, returns the count of values
// received (0 on timeout).  Submodule returns ALL its sensor readings
// in a single response; the caller doesn't need to specify a pin.
int       espnow_comm_request_read(uint8_t module_id, double* out_values, int max_values);

// Response handler — design.md §7.7
void      espnow_comm_handle_resp(const uint8_t* src_mac, const uint8_t* data, int len);

// Send command (master → sensor)
esp_err_t espnow_comm_send_cmd(uint8_t module_id, uint16_t cmd_id, const uint8_t* payload, uint8_t payload_len);

#define ESPNOW_COMMAND_PREVIEW_MAX 8

typedef enum {
    ESPNOW_COMMAND_NONE = 0,
    ESPNOW_COMMAND_PENDING,
    ESPNOW_COMMAND_CONFIRMED,
    ESPNOW_COMMAND_SEND_FAILED,
    ESPNOW_COMMAND_TIMED_OUT,
} espnow_command_state_t;

typedef struct {
    bool valid;
    uint8_t module_id;
    uint8_t seq_id;
    uint16_t cmd_id;
    uint8_t payload[ESPNOW_COMMAND_PREVIEW_MAX];
    uint8_t payload_len;
    espnow_command_state_t state;
    esp_err_t error;
    uint32_t sent_at_ms;
    uint32_t timeout_ms;
} espnow_command_status_t;

// Returns a snapshot of the latest command sent to one module.  Pending
// commands are changed to TIMED_OUT when their command-specific deadline has
// elapsed.  Returns false when no command has been sent to the module yet.
bool espnow_comm_get_command_status(uint8_t module_id,
                                   espnow_command_status_t* out_status);

// Send announce (sensor → broadcast)
void      espnow_comm_send_announce(void);

// Master discovery broadcast and radio-channel reconciliation.  These are
// used after SoftAP/channel transitions to bring ESP-NOW peers back online.
void      espnow_comm_send_discovery(void);
void      espnow_comm_sync_rf(void);

// Callback for received CMD/DATA_REQ (sensor mode)
typedef void (*espnow_recv_callback_t)(const uint8_t* src_mac, uint8_t msg_type,
                                       const uint8_t* data, int len);
void      espnow_comm_register_recv_callback(espnow_recv_callback_t cb);

// Module name — set by sensor before calling espnow_comm_send_announce()
extern char    g_espnow_module_name[17];

// Module capability descriptor — describes sensor function/data format
// Set by sensor along with g_espnow_module_name, sent in MSG_ANNOUNCE.
extern char    g_espnow_module_capability[CONFIG_MAX_CAPABILITY_LEN];

// Suspend/resume RX processing (used by wifi_scan during Wi-Fi scanning)
void espnow_comm_suspend_rx(void);
void espnow_comm_resume_rx(void);

// Re-initialise ESP-NOW layer after WiFi stop/start cycle.
// Re-registers callbacks, re-adds broadcast peer.
// Call after esp_wifi_start() + esp_wifi_set_channel().
esp_err_t espnow_comm_reinit_espnow(void);

// Align Wi-Fi channel and broadcast peer after SoftAP / channel changes.
void espnow_comm_sync_rf(void);

// Master-only: broadcast discovery so sensors re-announce immediately.
void espnow_comm_send_discovery(void);

#ifdef __cplusplus
}
#endif
