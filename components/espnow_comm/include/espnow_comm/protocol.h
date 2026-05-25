#pragma once

#include "sdkconfig.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ESP-NOW message types — design.md §7.2
#define MSG_VERSION      0x01

typedef enum {
    MSG_ANNOUNCE   = 0x10,
    MSG_CMD        = 0x20,
    MSG_DATA_REQ   = 0x30,
    MSG_DATA_RESP  = 0x40,
    MSG_ACK        = 0x50,
} MsgType;

// ── Data exchange (msg_type-driven) ──
// DATA_REQ → submodule reads ALL its sensors → replies DATA_RESP
//            cmd_id is unused (set to 0), payload is empty.
// CMD      → submodule dispatches on cmd_id (see §Command IDs below)

// ── Command IDs (0x0001–0xFFFF, only meaningful for MSG_CMD) ──
// Define custom commands here and add handler branches in the
// submodule's cmd_task.  Examples:
//   #define CMD_MOTOR_STOP   0x0001
//   #define CMD_LED_POWER_ON 0x0002

// Message header — design.md §7.2
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint8_t  target_id;
    uint8_t  seq_id;
    uint16_t cmd_id;
    uint8_t  payload_len;
} MsgHeader;

#define MSG_HEADER_SIZE sizeof(MsgHeader)

// ── DATA_RESP payload format ──
// [1B: value_count][8B × N: double values]
// Submodule packs all its sensor readings into a single response.
// The master parses the array and returns it to the script as a list.
#define DATA_RESP_MAX_VALUES 16        // max doubles per response
#define DATA_RESP_COUNT_SIZE 1
#define DATA_RESP_PAYLOAD_MIN_SIZE (DATA_RESP_COUNT_SIZE + 0)           // 0 values
#define DATA_RESP_PAYLOAD_MAX_SIZE (DATA_RESP_COUNT_SIZE + DATA_RESP_MAX_VALUES * 8)

// Packet builders
void protocol_build_announce(uint8_t* buf, size_t* len, const char* name);
void protocol_build_data_req(uint8_t* buf, size_t* len, uint8_t target_id, uint8_t seq_id);
void protocol_build_data_resp(uint8_t* buf, size_t* len, uint8_t target_id, uint8_t seq_id, const double* values, uint8_t value_count);
void protocol_build_ack(uint8_t* buf, size_t* len, uint8_t target_id, uint8_t seq_id);
void protocol_build_cmd(uint8_t* buf, size_t* len, uint8_t target_id, uint8_t seq_id, uint16_t cmd_id, const uint8_t* payload, uint8_t payload_len);

// Packet parsers
bool protocol_parse_header(const uint8_t* data, int len, MsgHeader* header_out);
int  protocol_extract_values(const uint8_t* data, int len, double* out_values, int max_values);

#ifdef __cplusplus
}
#endif
