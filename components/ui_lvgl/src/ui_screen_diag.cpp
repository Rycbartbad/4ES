#include "sdkconfig.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_timer.h"
#include "lvgl.h"

#include "ui_lvgl_internal.h"

static constexpr uint32_t DETAIL_HISTORY_WINDOW_MS = 120000;

static UiStatusState* s_state = NULL;
static UiSensorCard s_selected_card = {};
static volatile uint8_t s_selected_module_id = 0;

static lv_obj_t* s_overview = NULL;
static lv_obj_t* s_count_label = NULL;
static lv_obj_t* s_cards[UI_SENSOR_CARD_MAX] = {};
static lv_obj_t* s_name_labels[UI_SENSOR_CARD_MAX] = {};
static lv_obj_t* s_data_labels[UI_SENSOR_CARD_MAX] = {};
static lv_obj_t* s_state_labels[UI_SENSOR_CARD_MAX] = {};

static lv_obj_t* s_detail = NULL;
static lv_obj_t* s_detail_content = NULL;
static lv_obj_t* s_detail_title = NULL;
static lv_obj_t* s_detail_status = NULL;
static lv_obj_t* s_detail_age = NULL;
static lv_obj_t* s_detail_empty = NULL;
static lv_obj_t* s_metric_cards[UI_SENSOR_VALUE_MAX] = {};
static lv_obj_t* s_metric_names[UI_SENSOR_VALUE_MAX] = {};
static lv_obj_t* s_metric_values[UI_SENSOR_VALUE_MAX] = {};
static lv_obj_t* s_metric_charts[UI_SENSOR_VALUE_MAX] = {};
static lv_chart_series_t* s_metric_series[UI_SENSOR_VALUE_MAX] = {};

static lv_style_t s_style_screen;
static lv_style_t s_style_title;
static lv_style_t s_style_card_online;
static lv_style_t s_style_card_offline;
static lv_style_t s_style_metric_card;
static lv_style_t s_style_name;
static lv_style_t s_style_data;
static lv_style_t s_style_state;

static void set_card_style(lv_obj_t* card, bool connected)
{
    lv_obj_remove_style(card, &s_style_card_online, LV_PART_MAIN);
    lv_obj_remove_style(card, &s_style_card_offline, LV_PART_MAIN);
    lv_obj_add_style(card,
                     connected ? &s_style_card_online : &s_style_card_offline,
                     LV_PART_MAIN);
}

static const UiSensorCard* find_selected_card(const UiStatusState* state)
{
    if (state == NULL || s_selected_module_id == 0) {
        return NULL;
    }
    uint8_t module_ids[UI_SENSOR_CARD_MAX] = {};
    for (int i = 0; i < UI_SENSOR_CARD_MAX; i++) {
        module_ids[i] = state->sensors[i].present
                            ? state->sensors[i].module_id
                            : 0;
    }
    const int index = ui_sensor_find_module_index(
        module_ids, UI_SENSOR_CARD_MAX, s_selected_module_id);
    return index >= 0 ? &state->sensors[index] : NULL;
}

static void close_detail(void)
{
    s_selected_module_id = 0;
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_overview, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(s_detail_content, 0, LV_ANIM_OFF);
}

static void close_event_cb(lv_event_t* event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        close_detail();
    }
}

static void detail_gesture_cb(lv_event_t* event)
{
    if (lv_event_get_code(event) != LV_EVENT_GESTURE) {
        return;
    }
    lv_indev_t* indev = lv_indev_get_act();
    if (indev != NULL && lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) {
        close_detail();
    }
}

static void card_event_cb(lv_event_t* event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_state == NULL) {
        return;
    }
    const int index = (int)(uintptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= UI_SENSOR_CARD_MAX ||
        !s_state->sensors[index].present) {
        return;
    }

    s_selected_card = s_state->sensors[index];
    s_selected_module_id = s_selected_card.module_id;
    lv_obj_add_flag(s_overview, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(s_detail_content, 0, LV_ANIM_OFF);
}

static void update_chart(int metric_index, uint8_t module_id,
                         bool binary, uint32_t now_ms)
{
    float history[UI_SENSOR_HISTORY_CAPACITY] = {};
    const int count = ui_lvgl_copy_sensor_history(
        module_id, metric_index, now_ms, DETAIL_HISTORY_WINDOW_MS,
        history, UI_SENSOR_HISTORY_CAPACITY);

    lv_chart_set_all_value(s_metric_charts[metric_index],
                           s_metric_series[metric_index],
                           LV_CHART_POINT_NONE);
    if (count <= 0) {
        lv_chart_refresh(s_metric_charts[metric_index]);
        return;
    }

    lv_coord_t min_value = binary ? 0 : (lv_coord_t)(history[0] * 10.0f);
    lv_coord_t max_value = binary ? 10 : min_value;
    for (int i = 0; i < count; i++) {
        const lv_coord_t value = (lv_coord_t)(history[i] * 10.0f);
        if (value < min_value) min_value = value;
        if (value > max_value) max_value = value;
        const uint16_t point =
            (uint16_t)(UI_SENSOR_HISTORY_CAPACITY - count + i);
        lv_chart_set_value_by_id(s_metric_charts[metric_index],
                                 s_metric_series[metric_index], point, value);
    }

    if (!binary) {
        lv_coord_t span = max_value - min_value;
        lv_coord_t padding = span / 10;
        if (padding < 1) {
            padding = (lv_coord_t)(fabsf((float)max_value) / 10.0f);
            if (padding < 1) padding = 1;
        }
        min_value -= padding;
        max_value += padding;
    }
    lv_chart_set_range(s_metric_charts[metric_index],
                       LV_CHART_AXIS_PRIMARY_Y, min_value, max_value);
    lv_chart_refresh(s_metric_charts[metric_index]);
}

static void update_detail(const UiStatusState* state)
{
    if (s_selected_module_id == 0) {
        return;
    }

    const UiSensorCard* current = find_selected_card(state);
    if (current != NULL) {
        s_selected_card = *current;
    } else {
        s_selected_card.connected = false;
    }

    lv_label_set_text_fmt(s_detail_title, "%s  #%u", s_selected_card.name,
                          (unsigned)s_selected_card.module_id);
    lv_label_set_text(s_detail_status,
                      s_selected_card.connected ? "ONLINE" : "OFFLINE");
    lv_obj_set_style_text_color(
        s_detail_status,
        lv_color_hex(s_selected_card.connected ? 0x52D681 : 0xFF6B67),
        LV_PART_MAIN);

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const bool has_measurements = ui_sensor_capability_has_measurements(
        s_selected_card.capability);
    if (!has_measurements) {
        lv_label_set_text(s_detail_age, "Control device");
    } else if (s_selected_card.last_update_ms == 0) {
        lv_label_set_text(s_detail_age, "Waiting for data");
    } else {
        const uint32_t age_s =
            (now_ms - s_selected_card.last_update_ms) / 1000;
        lv_label_set_text_fmt(s_detail_age, "Updated %lus ago",
                              (unsigned long)age_s);
    }

    const int value_count = s_selected_card.value_count;
    if (value_count <= 0) {
        lv_label_set_text(s_detail_empty,
                          has_measurements ? "Waiting for sensor data..."
                                           : "Actuator device\nNo trend data");
        lv_obj_clear_flag(s_detail_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_detail_empty, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < UI_SENSOR_VALUE_MAX; i++) {
        if (i >= value_count) {
            lv_obj_add_flag(s_metric_cards[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_metric_cards[i], LV_OBJ_FLAG_HIDDEN);

        ui_metric_meta_t meta = {};
        ui_sensor_metric_meta(s_selected_card.capability, i, &meta);
        lv_label_set_text(s_metric_names[i], meta.label);
        if (meta.binary) {
            lv_label_set_text_fmt(s_metric_values[i], "%s  (%.0f)",
                                  s_selected_card.values[i] >= 0.5 ? "ON" : "OFF",
                                  s_selected_card.values[i]);
        } else if (meta.unit[0] != '\0') {
            lv_label_set_text_fmt(s_metric_values[i], "%.1f %s",
                                  s_selected_card.values[i], meta.unit);
        } else {
            lv_label_set_text_fmt(s_metric_values[i], "%.1f",
                                  s_selected_card.values[i]);
        }
        update_chart(i, s_selected_card.module_id, meta.binary, now_ms);
    }
}

static void create_overview(lv_obj_t* screen)
{
    s_overview = lv_obj_create(screen);
    lv_obj_set_size(s_overview, 240, 240);
    lv_obj_set_pos(s_overview, 0, 0);
    lv_obj_set_style_bg_opa(s_overview, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_overview, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_overview, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_overview, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(s_overview);
    lv_obj_add_style(title, &s_style_title, LV_PART_MAIN);
    lv_label_set_text(title, "ESP-LEGO Devices");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 6, 4);

    s_count_label = lv_label_create(s_overview);
    lv_obj_add_style(s_count_label, &s_style_title, LV_PART_MAIN);
    lv_obj_set_width(s_count_label, 58);
    lv_label_set_long_mode(s_count_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_count_label, LV_ALIGN_TOP_RIGHT, -6, 4);

    const lv_coord_t margin = 6;
    const lv_coord_t gap = 6;
    const lv_coord_t top = 25;
    const lv_coord_t card_w = 111;
    const lv_coord_t card_h = 101;

    for (int i = 0; i < UI_SENSOR_CARD_MAX; i++) {
        const int col = i % 2;
        const int row = i / 2;
        s_cards[i] = lv_obj_create(s_overview);
        lv_obj_set_pos(s_cards[i], margin + col * (card_w + gap),
                       top + row * (card_h + gap));
        lv_obj_set_size(s_cards[i], card_w, card_h);
        lv_obj_clear_flag(s_cards[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_cards[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_cards[i], card_event_cb, LV_EVENT_CLICKED,
                            (void*)(uintptr_t)i);
        set_card_style(s_cards[i], false);

        s_name_labels[i] = lv_label_create(s_cards[i]);
        lv_obj_add_style(s_name_labels[i], &s_style_name, LV_PART_MAIN);
        lv_obj_set_width(s_name_labels[i], card_w - 14);
        lv_label_set_long_mode(s_name_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_align(s_name_labels[i], LV_ALIGN_TOP_LEFT, 0, 0);

        s_data_labels[i] = lv_label_create(s_cards[i]);
        lv_obj_add_style(s_data_labels[i], &s_style_data, LV_PART_MAIN);
        lv_obj_set_width(s_data_labels[i], card_w - 14);
        lv_label_set_long_mode(s_data_labels[i], LV_LABEL_LONG_WRAP);
        lv_obj_align(s_data_labels[i], LV_ALIGN_TOP_LEFT, 0, 33);

        s_state_labels[i] = lv_label_create(s_cards[i]);
        lv_obj_add_style(s_state_labels[i], &s_style_state, LV_PART_MAIN);
        lv_obj_set_width(s_state_labels[i], card_w - 14);
        lv_label_set_long_mode(s_state_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_align(s_state_labels[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
}

static void create_detail(lv_obj_t* screen)
{
    s_detail = lv_obj_create(screen);
    lv_obj_set_size(s_detail, 240, 240);
    lv_obj_set_pos(s_detail, 0, 0);
    lv_obj_set_style_bg_color(s_detail, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_detail, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_detail, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_detail, detail_gesture_cb, LV_EVENT_GESTURE, NULL);

    s_detail_title = lv_label_create(s_detail);
    lv_obj_add_style(s_detail_title, &s_style_title, LV_PART_MAIN);
    lv_obj_set_width(s_detail_title, 164);
    lv_label_set_long_mode(s_detail_title, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(s_detail_title, 6, 3);

    s_detail_status = lv_label_create(s_detail);
    lv_obj_set_pos(s_detail_status, 6, 23);
    s_detail_age = lv_label_create(s_detail);
    lv_obj_set_pos(s_detail_age, 74, 23);
    lv_obj_set_style_text_color(s_detail_age, lv_color_hex(0xAAB4BE),
                                LV_PART_MAIN);

    lv_obj_t* close_button = lv_btn_create(s_detail);
    lv_obj_set_size(close_button, 38, 38);
    lv_obj_align(close_button, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_obj_add_event_cb(close_button, close_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* close_label = lv_label_create(close_button);
    lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
    lv_obj_center(close_label);

    s_detail_content = lv_obj_create(s_detail);
    lv_obj_set_pos(s_detail_content, 0, 43);
    lv_obj_set_size(s_detail_content, 240, 197);
    lv_obj_set_scroll_dir(s_detail_content, LV_DIR_VER);
    lv_obj_set_style_bg_opa(s_detail_content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_detail_content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_detail_content, 5, LV_PART_MAIN);
    lv_obj_add_event_cb(s_detail_content, detail_gesture_cb,
                        LV_EVENT_GESTURE, NULL);

    s_detail_empty = lv_label_create(s_detail_content);
    lv_label_set_text(s_detail_empty, "Waiting for sensor data...");
    lv_obj_set_style_text_color(s_detail_empty, lv_color_hex(0xAAB4BE),
                                LV_PART_MAIN);
    lv_obj_align(s_detail_empty, LV_ALIGN_TOP_MID, 0, 30);

    for (int i = 0; i < UI_SENSOR_VALUE_MAX; i++) {
        s_metric_cards[i] = lv_obj_create(s_detail_content);
        lv_obj_add_style(s_metric_cards[i], &s_style_metric_card,
                         LV_PART_MAIN);
        lv_obj_set_pos(s_metric_cards[i], 0, i * 142);
        lv_obj_set_size(s_metric_cards[i], 224, 134);
        lv_obj_clear_flag(s_metric_cards[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_metric_cards[i], detail_gesture_cb,
                            LV_EVENT_GESTURE, NULL);

        s_metric_names[i] = lv_label_create(s_metric_cards[i]);
        lv_obj_set_pos(s_metric_names[i], 0, 0);
        s_metric_values[i] = lv_label_create(s_metric_cards[i]);
        lv_obj_align(s_metric_values[i], LV_ALIGN_TOP_RIGHT, 0, 0);

        s_metric_charts[i] = lv_chart_create(s_metric_cards[i]);
        lv_obj_set_pos(s_metric_charts[i], 0, 25);
        lv_obj_set_size(s_metric_charts[i], 208, 94);
        lv_obj_add_event_cb(s_metric_charts[i], detail_gesture_cb,
                            LV_EVENT_GESTURE, NULL);
        lv_chart_set_type(s_metric_charts[i], LV_CHART_TYPE_LINE);
        lv_chart_set_point_count(s_metric_charts[i],
                                 UI_SENSOR_HISTORY_CAPACITY);
        lv_chart_set_div_line_count(s_metric_charts[i], 3, 4);
        s_metric_series[i] = lv_chart_add_series(
            s_metric_charts[i], lv_color_hex(0x55C7FF),
            LV_CHART_AXIS_PRIMARY_Y);
        lv_obj_add_flag(s_metric_cards[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
}

void ui_screen_diag_create(UiStatusState* state)
{
    s_state = state;

    lv_style_init(&s_style_screen);
    lv_style_set_bg_color(&s_style_screen, lv_color_hex(0x101418));
    lv_style_set_bg_opa(&s_style_screen, LV_OPA_COVER);
    lv_style_init(&s_style_title);
    lv_style_set_text_color(&s_style_title, lv_color_hex(0xF8FAFC));

    lv_style_init(&s_style_card_online);
    lv_style_set_bg_color(&s_style_card_online, lv_color_hex(0x1F8A4C));
    lv_style_set_bg_opa(&s_style_card_online, LV_OPA_COVER);
    lv_style_set_border_color(&s_style_card_online, lv_color_hex(0xD8F8E4));
    lv_style_set_border_width(&s_style_card_online, 2);
    lv_style_set_radius(&s_style_card_online, 3);
    lv_style_set_pad_all(&s_style_card_online, 5);

    lv_style_init(&s_style_card_offline);
    lv_style_set_bg_color(&s_style_card_offline, lv_color_hex(0xB73632));
    lv_style_set_bg_opa(&s_style_card_offline, LV_OPA_COVER);
    lv_style_set_border_color(&s_style_card_offline, lv_color_hex(0xF6D3D0));
    lv_style_set_border_width(&s_style_card_offline, 2);
    lv_style_set_radius(&s_style_card_offline, 3);
    lv_style_set_pad_all(&s_style_card_offline, 5);

    lv_style_init(&s_style_metric_card);
    lv_style_set_bg_color(&s_style_metric_card, lv_color_hex(0x1B232B));
    lv_style_set_bg_opa(&s_style_metric_card, LV_OPA_COVER);
    lv_style_set_border_color(&s_style_metric_card, lv_color_hex(0x344453));
    lv_style_set_border_width(&s_style_metric_card, 1);
    lv_style_set_radius(&s_style_metric_card, 5);
    lv_style_set_pad_all(&s_style_metric_card, 7);

    lv_style_init(&s_style_name);
    lv_style_set_text_color(&s_style_name, lv_color_hex(0xFFFFFF));
    lv_style_init(&s_style_data);
    lv_style_set_text_color(&s_style_data, lv_color_hex(0xFFFFFF));
    lv_style_init(&s_style_state);
    lv_style_set_text_color(&s_style_state, lv_color_hex(0xFFFFFF));

    lv_obj_t* screen = lv_scr_act();
    lv_obj_add_style(screen, &s_style_screen, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    create_overview(screen);
    create_detail(screen);
}

void ui_screen_diag_update(const UiStatusState* state)
{
    if (state == NULL || s_count_label == NULL) {
        return;
    }

    lv_label_set_text_fmt(s_count_label, "%d", state->total_sensor_count);
    for (int i = 0; i < UI_SENSOR_CARD_MAX; i++) {
        const UiSensorCard* card = &state->sensors[i];
        set_card_style(s_cards[i], card->connected);
        lv_label_set_text(s_name_labels[i], card->name);
        lv_label_set_text_fmt(s_data_labels[i], "Data\n%s", card->data);
        lv_label_set_text(s_state_labels[i],
                          card->connected ? "ONLINE" : "OFFLINE");
        if (card->present) {
            lv_obj_clear_state(s_cards[i], LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_cards[i], LV_STATE_DISABLED);
        }
    }
    update_detail(state);
}

uint8_t ui_screen_diag_selected_module(void)
{
    return s_selected_module_id;
}
