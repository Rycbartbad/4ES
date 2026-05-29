#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

#include "espnow_comm/peer_mgr.h"
#include "ui_lvgl_internal.h"

static void copy_text(char* dst, size_t dst_len, const char* src)
{
    if (dst == NULL || dst_len == 0) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static void format_values(const PeerEntry* peer, char* out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }

    if (peer == NULL || peer->last_value_count == 0) {
        copy_text(out, out_len, "--");
        return;
    }

    size_t used = 0;
    out[0] = '\0';
    for (uint8_t i = 0; i < peer->last_value_count; i++) {
        int written = snprintf(out + used, out_len - used,
                               (i == 0) ? "%.1f" : ", %.1f",
                               peer->last_values[i]);
        if (written < 0) {
            out[out_len - 1] = '\0';
            return;
        }
        size_t n = (size_t)written;
        if (n >= out_len - used) {
            out[out_len - 1] = '\0';
            return;
        }
        used += n;
    }
}

void ui_peer_view_refresh(UiStatusState* state)
{
    if (state == NULL) {
        return;
    }

    PeerEntry peers[UI_SENSOR_CARD_MAX];
    memset(peers, 0, sizeof(peers));
    memset(state, 0, sizeof(*state));

    int total = peer_mgr_copy_snapshot(peers, UI_SENSOR_CARD_MAX);
    int shown = total;
    if (shown > UI_SENSOR_CARD_MAX) {
        shown = UI_SENSOR_CARD_MAX;
    }

    state->total_sensor_count = total;

    for (int i = 0; i < shown; i++) {
        UiSensorCard* card = &state->sensors[i];
        card->present = true;
        card->connected = (peers[i].state == PEER_ACTIVE);
        copy_text(card->name, sizeof(card->name),
                  peers[i].name[0] ? peers[i].name : "sensor");
        format_values(&peers[i], card->data, sizeof(card->data));
    }

    for (int i = shown; i < UI_SENSOR_CARD_MAX; i++) {
        UiSensorCard* card = &state->sensors[i];
        card->present = false;
        card->connected = false;
        static const char* empty_names[UI_SENSOR_CARD_MAX] = {
            "sensor 1", "sensor 2", "sensor 3", "sensor 4",
        };
        copy_text(card->name, sizeof(card->name), empty_names[i]);
        copy_text(card->data, sizeof(card->data), "--");
    }
}
