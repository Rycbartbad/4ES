/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO V1.0 — ESP-NOW protocol packet construction / parsing helpers.
 */

#include "sdkconfig.h"
#include "espnow_comm/protocol.h"
#include <string.h>

// ----------------------------------------------------------------
// Packet builders
// ----------------------------------------------------------------

void protocol_build_announce(uint8_t* buf, size_t* len, uint8_t module_id, const char* name)
{
    MsgHeader* hdr = (MsgHeader*)buf;
    hdr->version   = MSG_VERSION;
    hdr->msg_type  = MSG_ANNOUNCE;
    hdr->target_id = 0xFF;          // broadcast
    hdr->seq_id    = 0;
    hdr->cmd_id    = 0;

    // Payload: [1B module_id][16B name, zero-padded]
    uint8_t* pay = buf + MSG_HEADER_SIZE;
    pay[0] = module_id;

    size_t name_len = strlen(name);
    if (name_len > 16) name_len = 16;
    memcpy(pay + 1, name, name_len);
    if (name_len < 16) {
        memset(pay + 1 + name_len, 0, 16 - name_len);
    }

    hdr->payload_len = 1 + 16;      // fixed-size announce payload
    *len = MSG_HEADER_SIZE + 1 + 16;
}

void protocol_build_data_req(uint8_t* buf, size_t* len,
                             uint8_t target_id, uint8_t seq_id, uint8_t pin)
{
    MsgHeader* hdr = (MsgHeader*)buf;
    hdr->version   = MSG_VERSION;
    hdr->msg_type  = MSG_DATA_REQ;
    hdr->target_id = target_id;
    hdr->seq_id    = seq_id;
    hdr->cmd_id    = CMD_READ_SENSOR;
    hdr->payload_len = DATA_REQ_PIN_SIZE;

    buf[MSG_HEADER_SIZE] = pin;
    *len = MSG_HEADER_SIZE + DATA_REQ_PIN_SIZE;
}

void protocol_build_data_resp(uint8_t* buf, size_t* len,
                              uint8_t target_id, uint8_t seq_id, double value)
{
    MsgHeader* hdr = (MsgHeader*)buf;
    hdr->version   = MSG_VERSION;
    hdr->msg_type  = MSG_DATA_RESP;
    hdr->target_id = target_id;
    hdr->seq_id    = seq_id;
    hdr->cmd_id    = 0;
    hdr->payload_len = DATA_RESP_VAL_SIZE;

    memcpy(buf + MSG_HEADER_SIZE, &value, sizeof(double));
    *len = MSG_HEADER_SIZE + DATA_RESP_VAL_SIZE;
}

void protocol_build_ack(uint8_t* buf, size_t* len,
                        uint8_t target_id, uint8_t seq_id)
{
    MsgHeader* hdr = (MsgHeader*)buf;
    hdr->version   = MSG_VERSION;
    hdr->msg_type  = MSG_ACK;
    hdr->target_id = target_id;
    hdr->seq_id    = seq_id;
    hdr->cmd_id    = 0;
    hdr->payload_len = 0;

    *len = MSG_HEADER_SIZE;
}

void protocol_build_cmd(uint8_t* buf, size_t* len,
                        uint8_t target_id, uint8_t seq_id,
                        uint16_t cmd_id,
                        const uint8_t* payload, uint8_t payload_len)
{
    MsgHeader* hdr = (MsgHeader*)buf;
    hdr->version   = MSG_VERSION;
    hdr->msg_type  = MSG_CMD;
    hdr->target_id = target_id;
    hdr->seq_id    = seq_id;
    hdr->cmd_id    = cmd_id;
    hdr->payload_len = payload_len;

    if (payload_len > 0 && payload != NULL) {
        memcpy(buf + MSG_HEADER_SIZE, payload, payload_len);
    }
    *len = MSG_HEADER_SIZE + payload_len;
}

// ----------------------------------------------------------------
// Packet parsers
// ----------------------------------------------------------------

bool protocol_parse_header(const uint8_t* data, int len, MsgHeader* header_out)
{
    if (data == NULL || header_out == NULL) {
        return false;
    }
    if (len < (int)MSG_HEADER_SIZE) {
        return false;
    }

    const MsgHeader* hdr = (const MsgHeader*)data;
    if (hdr->version != MSG_VERSION) {
        return false;
    }

    memcpy(header_out, hdr, MSG_HEADER_SIZE);
    return true;
}

bool protocol_extract_double(const uint8_t* data, int len, double* value_out)
{
    if (data == NULL || value_out == NULL) {
        return false;
    }
    if (len < (int)(MSG_HEADER_SIZE + DATA_RESP_VAL_SIZE)) {
        return false;
    }

    memcpy(value_out, data + MSG_HEADER_SIZE, sizeof(double));
    return true;
}
