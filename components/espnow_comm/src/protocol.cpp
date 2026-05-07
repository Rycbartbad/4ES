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

void protocol_build_announce(uint8_t* buf, size_t* len, const char* name)
{
    MsgHeader* hdr = (MsgHeader*)buf;
    hdr->version   = MSG_VERSION;
    hdr->msg_type  = MSG_ANNOUNCE;
    hdr->target_id = 0xFF;          // broadcast
    hdr->seq_id    = 0;
    hdr->cmd_id    = 0;

    // Payload: [16B name, zero-padded] (no module_id — master assigns it)
    uint8_t* pay = buf + MSG_HEADER_SIZE;

    size_t name_len = strlen(name);
    if (name_len > 16) name_len = 16;
    memcpy(pay, name, name_len);
    if (name_len < 16) {
        memset(pay + name_len, 0, 16 - name_len);
    }

    hdr->payload_len = 16;          // name only
    *len = MSG_HEADER_SIZE + 16;
}

void protocol_build_data_req(uint8_t* buf, size_t* len,
                             uint8_t target_id, uint8_t seq_id)
{
    MsgHeader* hdr = (MsgHeader*)buf;
    hdr->version   = MSG_VERSION;
    hdr->msg_type  = MSG_DATA_REQ;
    hdr->target_id = target_id;
    hdr->seq_id    = seq_id;
    hdr->cmd_id    = 0;
    hdr->payload_len = 0;           // no payload — submodule reads all its sensors

    *len = MSG_HEADER_SIZE;
}

void protocol_build_data_resp(uint8_t* buf, size_t* len,
                              uint8_t target_id, uint8_t seq_id,
                              const double* values, uint8_t value_count)
{
    MsgHeader* hdr = (MsgHeader*)buf;
    hdr->version    = MSG_VERSION;
    hdr->msg_type   = MSG_DATA_RESP;
    hdr->target_id  = target_id;
    hdr->seq_id     = seq_id;
    hdr->cmd_id     = 0;

    uint8_t* pay = buf + MSG_HEADER_SIZE;
    pay[0] = value_count;
    for (uint8_t i = 0; i < value_count && i < DATA_RESP_MAX_VALUES; i++) {
        memcpy(pay + DATA_RESP_COUNT_SIZE + i * 8, &values[i], 8);
    }

    hdr->payload_len = DATA_RESP_COUNT_SIZE + value_count * 8;
    *len = MSG_HEADER_SIZE + hdr->payload_len;
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

int protocol_extract_values(const uint8_t* data, int len,
                            double* out_values, int max_values)
{
    if (data == NULL || out_values == NULL || max_values <= 0) {
        return 0;
    }

    int payload_avail = len - (int)MSG_HEADER_SIZE;
    if (payload_avail < DATA_RESP_COUNT_SIZE) {
        return 0;
    }

    const uint8_t* pay = data + MSG_HEADER_SIZE;
    uint8_t count = pay[0];

    int expected = DATA_RESP_COUNT_SIZE + count * 8;
    if (payload_avail < expected) {
        return 0;   // truncated
    }
    if (count > DATA_RESP_MAX_VALUES) {
        count = DATA_RESP_MAX_VALUES;  // clamp
    }

    int n = (count < max_values) ? count : max_values;
    for (int i = 0; i < n; i++) {
        memcpy(&out_values[i], pay + DATA_RESP_COUNT_SIZE + i * 8, 8);
    }
    return n;
}
