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

    if (peer == NULL) {
        copy_text(out, out_len, "--");
        return;
    }

    double values[UI_SENSOR_VALUE_MAX];
    int value_count = ui_lvgl_copy_sensor_values(peer->module_id, values,
                                                 UI_SENSOR_VALUE_MAX);
    if (value_count <= 0) {
        copy_text(out, out_len, "--");
        return;
    }

    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < value_count; i++) {
        int written = snprintf(out + used, out_len - used,
                               (i == 0) ? "%.1f" : ", %.1f",
                               values[i]);
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

    memset(state, 0, sizeof(*state));

    int total = 0;
    PeerEntry** peers = peer_mgr_list(&total);
    int shown = total;
    if (shown > UI_SENSOR_CARD_MAX) {
        shown = UI_SENSOR_CARD_MAX;
    }

    state->total_sensor_count = total;

    for (int i = 0; i < shown; i++) {
        const PeerEntry* peer = peers ? peers[i] : NULL;
        UiSensorCard* card = &state->sensors[i];
        card->present = true;
        card->connected = (peer != NULL && peer->state == PEER_ACTIVE);
        copy_text(card->name, sizeof(card->name),
                  (peer && peer->name[0]) ? peer->name : "sensor");
        format_values(peer, card->data, sizeof(card->data));
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
