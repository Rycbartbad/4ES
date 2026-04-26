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

// Command IDs
#define CMD_READ_SENSOR  0x0001

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

// Payload formats — design.md §7.4
// DATA_REQ:  1 byte (pin number)
// DATA_RESP: 8 bytes (double value)
#define DATA_REQ_PIN_SIZE  1
#define DATA_RESP_VAL_SIZE 8

// Packet builders
void protocol_build_announce(uint8_t* buf, size_t* len, uint8_t module_id, const char* name);
void protocol_build_data_req(uint8_t* buf, size_t* len, uint8_t target_id, uint8_t seq_id, uint8_t pin);
void protocol_build_data_resp(uint8_t* buf, size_t* len, uint8_t target_id, uint8_t seq_id, double value);
void protocol_build_ack(uint8_t* buf, size_t* len, uint8_t target_id, uint8_t seq_id);
void protocol_build_cmd(uint8_t* buf, size_t* len, uint8_t target_id, uint8_t seq_id, uint16_t cmd_id, const uint8_t* payload, uint8_t payload_len);

// Packet parsers
bool protocol_parse_header(const uint8_t* data, int len, MsgHeader* header_out);
bool protocol_extract_double(const uint8_t* data, int len, double* value_out);

#ifdef __cplusplus
}
#endif
