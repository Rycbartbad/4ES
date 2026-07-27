#include "sdkconfig.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "lvgl.h"

#include "espnow_comm/comm.h"
#include "ui_lvgl_internal.h"

#if CONFIG_DEVICE_ROLE_MASTER && !CONFIG_LV_SPRINTF_USE_FLOAT
#error "Sensor detail labels require CONFIG_LV_SPRINTF_USE_FLOAT"
#endif

static constexpr uint32_t DETAIL_HISTORY_WINDOW_MS = 120000;

static UiStatusState* s_state = NULL;
static UiDeviceCard s_selected_card = {};
static volatile uint8_t s_selected_module_id = 0;

typedef enum {
    UI_CATEGORY_SENSORS = 0,
    UI_CATEGORY_ACTUATORS,
} UiDeviceCategory;

static UiDeviceCategory s_category = UI_CATEGORY_SENSORS;
static lv_obj_t* s_home = NULL;
static lv_obj_t* s_sensor_count = NULL;
static lv_obj_t* s_actuator_count = NULL;
static lv_obj_t* s_device_list = NULL;
static lv_obj_t* s_list_content = NULL;
static lv_obj_t* s_list_title = NULL;
static lv_obj_t* s_list_empty = NULL;
static lv_obj_t* s_cards[UI_DEVICE_MAX] = {};
static lv_obj_t* s_name_labels[UI_DEVICE_MAX] = {};
static lv_obj_t* s_data_labels[UI_DEVICE_MAX] = {};
static lv_obj_t* s_state_labels[UI_DEVICE_MAX] = {};
static uint8_t s_list_module_ids[UI_DEVICE_MAX] = {};

static lv_obj_t* s_detail = NULL;
static lv_obj_t* s_detail_content = NULL;
static lv_obj_t* s_detail_title = NULL;
static lv_obj_t* s_detail_status = NULL;
static lv_obj_t* s_detail_age = NULL;
static lv_obj_t* s_detail_empty = NULL;
static lv_obj_t* s_actuator_card = NULL;
static lv_obj_t* s_actuator_capability = NULL;
static lv_obj_t* s_actuator_command = NULL;
static lv_obj_t* s_actuator_result = NULL;
static lv_obj_t* s_servo_target = NULL;
static lv_obj_t* s_servo_slider = NULL;
static bool s_servo_dragging = false;
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

static bool capability_has_buzzer(const char* capability)
{
    return capability != NULL &&
           (strstr(capability, "buzzer_") != NULL ||
            strstr(capability, "Doorbell") != NULL ||
            strstr(capability, "Buzzer") != NULL);
}

static bool capability_has_servo(const char* capability)
{
    return capability != NULL &&
           (strstr(capability, "servo_write") != NULL ||
            strstr(capability, "Servo") != NULL);
}

static void format_command(const espnow_command_status_t* status,
                           char* out, size_t out_len)
{
    if (status == NULL || out == NULL || out_len == 0) {
        return;
    }
    switch (status->cmd_id) {
    case CMD_SERVO_WRITE:
        if (status->payload_len >= 1) {
            snprintf(out, out_len, "Set target: %u deg",
                     (unsigned)status->payload[0]);
            return;
        }
        break;
    case CMD_BUZZER_NOTE:
        if (status->payload_len >= 3) {
            const unsigned duration =
                ((unsigned)status->payload[1] << 8) | status->payload[2];
            snprintf(out, out_len, "Play note %u for %u ms",
                     (unsigned)status->payload[0], duration);
            return;
        }
        break;
    case CMD_BUZZER_SONG:
        if (status->payload_len >= 1) {
            snprintf(out, out_len, "Play song %u",
                     (unsigned)status->payload[0]);
            return;
        }
        break;
    case CMD_BUZZER_MELODY:
        snprintf(out, out_len, "Play melody");
        return;
    default:
        break;
    }
    snprintf(out, out_len, "Command 0x%04X", (unsigned)status->cmd_id);
}

static void servo_slider_event_cb(lv_event_t* event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        s_servo_dragging = true;
        return;
    }
    if (code == LV_EVENT_VALUE_CHANGED && s_servo_dragging) {
        lv_label_set_text_fmt(s_servo_target, "Selected: %d deg",
                              (int)lv_slider_get_value(s_servo_slider));
        return;
    }
    if (code != LV_EVENT_RELEASED || !s_servo_dragging) {
        return;
    }

    s_servo_dragging = false;
    if (!s_selected_card.connected ||
        !capability_has_servo(s_selected_card.capability)) {
        return;
    }
    const uint8_t angle = (uint8_t)lv_slider_get_value(s_servo_slider);
    lv_label_set_text_fmt(s_servo_target, "Target: %u deg (sending)",
                          (unsigned)angle);
    (void)espnow_comm_send_cmd(s_selected_card.module_id, CMD_SERVO_WRITE,
                              &angle, 1);
}

static void set_card_style(lv_obj_t* card, bool connected)
{
    lv_obj_remove_style(card, &s_style_card_online, LV_PART_MAIN);
    lv_obj_remove_style(card, &s_style_card_offline, LV_PART_MAIN);
    lv_obj_add_style(card,
                     connected ? &s_style_card_online : &s_style_card_offline,
                     LV_PART_MAIN);
}

static const UiDeviceCard* find_device_by_module(const UiStatusState* state,
                                                 uint8_t module_id)
{
    if (state == NULL || module_id == 0) {
        return NULL;
    }
    for (int i = 0; i < state->device_count; i++) {
        if (state->devices[i].present &&
            state->devices[i].module_id == module_id) {
            return &state->devices[i];
        }
    }
    return NULL;
}

static const UiDeviceCard* find_selected_card(const UiStatusState* state)
{
    return find_device_by_module(state, s_selected_module_id);
}

static void show_home(void)
{
    s_selected_module_id = 0;
    s_servo_dragging = false;
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_device_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_home, LV_OBJ_FLAG_HIDDEN);
}

static void show_device_list(void)
{
    s_selected_module_id = 0;
    s_servo_dragging = false;
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_device_list, LV_OBJ_FLAG_HIDDEN);
}

static void close_detail(void)
{
    show_device_list();
    lv_obj_scroll_to_y(s_detail_content, 0, LV_ANIM_OFF);
}

static void close_event_cb(lv_event_t* event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        close_detail();
    }
}

static void home_event_cb(lv_event_t* event)
{
    if (lv_event_get_code(event) != LV_EVENT_PRESSED) {
        return;
    }
    s_category = (UiDeviceCategory)(uintptr_t)lv_event_get_user_data(event);
    lv_obj_scroll_to_y(s_list_content, 0, LV_ANIM_OFF);
    ui_screen_diag_update(s_state);
    show_device_list();
}

static void home_back_event_cb(lv_event_t* event)
{
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        show_home();
    }
}

static void card_event_cb(lv_event_t* event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_state == NULL) {
        return;
    }
    const int index = (int)(uintptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= UI_DEVICE_MAX ||
        s_list_module_ids[index] == 0) {
        return;
    }

    const UiDeviceCard* card = find_device_by_module(
        s_state, s_list_module_ids[index]);
    if (card == NULL) {
        return;
    }
    s_selected_card = *card;
    s_selected_module_id = s_selected_card.module_id;
    lv_obj_add_flag(s_device_list, LV_OBJ_FLAG_HIDDEN);
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

    const UiDeviceCard* current = find_selected_card(state);
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
    const bool has_buzzer = capability_has_buzzer(s_selected_card.capability);
    const bool has_servo = capability_has_servo(s_selected_card.capability);
    const bool has_actuator = has_buzzer || has_servo;
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

    espnow_command_status_t command_status = {};
    const bool has_command = has_actuator && espnow_comm_get_command_status(
        s_selected_card.module_id, &command_status);

    if (has_actuator) {
        lv_obj_clear_flag(s_actuator_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_actuator_card, has_servo ? 166 : 108);
        if (has_servo && has_buzzer) {
            lv_label_set_text(s_actuator_capability,
                              "Capabilities: Servo, Buzzer");
        } else if (has_servo) {
            lv_label_set_text(s_actuator_capability, "Capability: Servo");
        } else {
            lv_label_set_text(s_actuator_capability, "Capability: Buzzer");
        }
        if (has_command) {
            char command_text[64] = {};
            format_command(&command_status, command_text,
                           sizeof(command_text));
            lv_label_set_text_fmt(s_actuator_command, "Last command\n%s",
                                  command_text);
            switch (command_status.state) {
            case ESPNOW_COMMAND_PENDING:
                lv_label_set_text(s_actuator_result, "Result: EXECUTING");
                lv_obj_set_style_text_color(s_actuator_result,
                                            lv_color_hex(0xFFC857),
                                            LV_PART_MAIN);
                break;
            case ESPNOW_COMMAND_CONFIRMED:
                lv_label_set_text(s_actuator_result, "Result: CONFIRMED");
                lv_obj_set_style_text_color(s_actuator_result,
                                            lv_color_hex(0x52D681),
                                            LV_PART_MAIN);
                break;
            case ESPNOW_COMMAND_SEND_FAILED:
                lv_label_set_text_fmt(s_actuator_result, "Result: SEND FAILED (%s)",
                                      esp_err_to_name(command_status.error));
                lv_obj_set_style_text_color(s_actuator_result,
                                            lv_color_hex(0xFF6B67),
                                            LV_PART_MAIN);
                break;
            case ESPNOW_COMMAND_TIMED_OUT:
                lv_label_set_text(s_actuator_result, "Result: NOT CONFIRMED");
                lv_obj_set_style_text_color(s_actuator_result,
                                            lv_color_hex(0xFF6B67),
                                            LV_PART_MAIN);
                break;
            default:
                lv_label_set_text(s_actuator_result, "Result: --");
                break;
            }
        } else {
            lv_label_set_text(s_actuator_command,
                              "Last command\nNo command yet");
            lv_label_set_text(s_actuator_result, "Result: --");
            lv_obj_set_style_text_color(s_actuator_result,
                                        lv_color_hex(0xAAB4BE), LV_PART_MAIN);
        }

        if (has_servo) {
            lv_obj_clear_flag(s_servo_target, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_servo_slider, LV_OBJ_FLAG_HIDDEN);
            if (s_selected_card.connected) {
                lv_obj_clear_state(s_servo_slider, LV_STATE_DISABLED);
            } else {
                lv_obj_add_state(s_servo_slider, LV_STATE_DISABLED);
                s_servo_dragging = false;
            }
            if (!s_servo_dragging && has_command &&
                command_status.cmd_id == CMD_SERVO_WRITE &&
                command_status.payload_len >= 1) {
                lv_slider_set_value(s_servo_slider,
                                    command_status.payload[0], LV_ANIM_OFF);
                lv_label_set_text_fmt(s_servo_target, "Target: %u deg",
                                      (unsigned)command_status.payload[0]);
            } else if (!s_servo_dragging &&
                       (!has_command || command_status.cmd_id != CMD_SERVO_WRITE)) {
                lv_slider_set_value(s_servo_slider, 90, LV_ANIM_OFF);
                lv_label_set_text(s_servo_target, "Target: not set");
            }
        } else {
            lv_obj_add_flag(s_servo_target, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_servo_slider, LV_OBJ_FLAG_HIDDEN);
            s_servo_dragging = false;
        }
    } else {
        lv_obj_add_flag(s_actuator_card, LV_OBJ_FLAG_HIDDEN);
        s_servo_dragging = false;
    }

    const int value_count = has_measurements ? s_selected_card.value_count : 0;
    if (value_count <= 0 && !has_actuator) {
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
        lv_obj_set_y(s_metric_cards[i],
                     (has_actuator ? (has_servo ? 174 : 116) : 0) + i * 142);

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
    s_home = lv_obj_create(screen);
    lv_obj_set_size(s_home, 240, 240);
    lv_obj_set_pos(s_home, 0, 0);
    lv_obj_set_style_bg_opa(s_home, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_home, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_home, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(s_home);
    lv_obj_add_style(title, &s_style_title, LV_PART_MAIN);
    lv_label_set_text(title, "ESP-LEGO Devices");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t* sensor_button = lv_btn_create(s_home);
    lv_obj_set_pos(sensor_button, 6, 43);
    lv_obj_set_size(sensor_button, 111, 174);
    lv_obj_add_event_cb(sensor_button, home_event_cb, LV_EVENT_PRESSED,
                        (void*)(uintptr_t)UI_CATEGORY_SENSORS);
    lv_obj_t* sensor_label = lv_label_create(sensor_button);
    lv_label_set_text(sensor_label, "Sensors");
    lv_obj_align(sensor_label, LV_ALIGN_TOP_MID, 0, 34);
    s_sensor_count = lv_label_create(sensor_button);
    lv_label_set_text(s_sensor_count, "0 devices");
    lv_obj_align(s_sensor_count, LV_ALIGN_BOTTOM_MID, 0, -34);

    lv_obj_t* actuator_button = lv_btn_create(s_home);
    lv_obj_set_pos(actuator_button, 123, 43);
    lv_obj_set_size(actuator_button, 111, 174);
    lv_obj_add_event_cb(actuator_button, home_event_cb, LV_EVENT_PRESSED,
                        (void*)(uintptr_t)UI_CATEGORY_ACTUATORS);
    lv_obj_t* actuator_label = lv_label_create(actuator_button);
    lv_label_set_text(actuator_label, "Actuators");
    lv_obj_align(actuator_label, LV_ALIGN_TOP_MID, 0, 34);
    s_actuator_count = lv_label_create(actuator_button);
    lv_label_set_text(s_actuator_count, "0 devices");
    lv_obj_align(s_actuator_count, LV_ALIGN_BOTTOM_MID, 0, -34);

    s_device_list = lv_obj_create(screen);
    lv_obj_set_size(s_device_list, 240, 240);
    lv_obj_set_pos(s_device_list, 0, 0);
    lv_obj_set_style_bg_color(s_device_list, lv_color_hex(0x101418),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_device_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_device_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_device_list, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_device_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* back_button = lv_btn_create(s_device_list);
    lv_obj_set_pos(back_button, 2, 2);
    lv_obj_set_size(back_button, 48, 38);
    lv_obj_add_event_cb(back_button, home_back_event_cb,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_t* back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_center(back_label);

    s_list_title = lv_label_create(s_device_list);
    lv_obj_add_style(s_list_title, &s_style_title, LV_PART_MAIN);
    lv_obj_set_pos(s_list_title, 46, 10);

    s_list_content = lv_obj_create(s_device_list);
    lv_obj_set_pos(s_list_content, 0, 38);
    lv_obj_set_size(s_list_content, 240, 202);
    lv_obj_set_scroll_dir(s_list_content, LV_DIR_VER);
    lv_obj_set_style_bg_opa(s_list_content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list_content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list_content, 5, LV_PART_MAIN);
    s_list_empty = lv_label_create(s_list_content);
    lv_label_set_text(s_list_empty, "No devices in this category");
    lv_obj_set_style_text_color(s_list_empty, lv_color_hex(0xAAB4BE),
                                LV_PART_MAIN);
    lv_obj_align(s_list_empty, LV_ALIGN_TOP_MID, 0, 55);

    for (int i = 0; i < UI_DEVICE_MAX; i++) {
        s_cards[i] = lv_obj_create(s_list_content);
        lv_obj_set_pos(s_cards[i], 0, i * 74);
        lv_obj_set_size(s_cards[i], 224, 68);
        lv_obj_clear_flag(s_cards[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_cards[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_cards[i], card_event_cb, LV_EVENT_CLICKED,
                            (void*)(uintptr_t)i);
        set_card_style(s_cards[i], false);

        s_name_labels[i] = lv_label_create(s_cards[i]);
        lv_obj_add_style(s_name_labels[i], &s_style_name, LV_PART_MAIN);
        lv_obj_set_width(s_name_labels[i], 145);
        lv_label_set_long_mode(s_name_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_align(s_name_labels[i], LV_ALIGN_TOP_LEFT, 0, 0);

        s_data_labels[i] = lv_label_create(s_cards[i]);
        lv_obj_add_style(s_data_labels[i], &s_style_data, LV_PART_MAIN);
        lv_obj_set_width(s_data_labels[i], 208);
        lv_label_set_long_mode(s_data_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_align(s_data_labels[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);

        s_state_labels[i] = lv_label_create(s_cards[i]);
        lv_obj_add_style(s_state_labels[i], &s_style_state, LV_PART_MAIN);
        lv_obj_set_width(s_state_labels[i], 62);
        lv_label_set_long_mode(s_state_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_align(s_state_labels[i], LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_add_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_move_foreground(back_button);
    lv_obj_add_flag(s_device_list, LV_OBJ_FLAG_HIDDEN);
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

    s_detail_empty = lv_label_create(s_detail_content);
    lv_label_set_text(s_detail_empty, "Waiting for sensor data...");
    lv_obj_set_style_text_color(s_detail_empty, lv_color_hex(0xAAB4BE),
                                LV_PART_MAIN);
    lv_obj_align(s_detail_empty, LV_ALIGN_TOP_MID, 0, 30);

    s_actuator_card = lv_obj_create(s_detail_content);
    lv_obj_add_style(s_actuator_card, &s_style_metric_card, LV_PART_MAIN);
    lv_obj_set_pos(s_actuator_card, 0, 0);
    lv_obj_set_size(s_actuator_card, 224, 166);
    lv_obj_clear_flag(s_actuator_card, LV_OBJ_FLAG_SCROLLABLE);

    s_actuator_capability = lv_label_create(s_actuator_card);
    lv_obj_set_pos(s_actuator_capability, 0, 0);
    lv_obj_set_width(s_actuator_capability, 208);
    lv_label_set_long_mode(s_actuator_capability, LV_LABEL_LONG_WRAP);

    s_actuator_command = lv_label_create(s_actuator_card);
    lv_obj_set_pos(s_actuator_command, 0, 28);
    lv_obj_set_width(s_actuator_command, 208);
    lv_label_set_long_mode(s_actuator_command, LV_LABEL_LONG_WRAP);

    s_actuator_result = lv_label_create(s_actuator_card);
    lv_obj_set_pos(s_actuator_result, 0, 72);
    lv_obj_set_width(s_actuator_result, 208);
    lv_label_set_long_mode(s_actuator_result, LV_LABEL_LONG_CLIP);

    s_servo_target = lv_label_create(s_actuator_card);
    lv_obj_set_pos(s_servo_target, 0, 101);
    s_servo_slider = lv_slider_create(s_actuator_card);
    lv_slider_set_range(s_servo_slider, 0, 180);
    lv_slider_set_value(s_servo_slider, 90, LV_ANIM_OFF);
    lv_obj_set_pos(s_servo_slider, 4, 132);
    lv_obj_set_size(s_servo_slider, 200, 14);
    lv_obj_add_event_cb(s_servo_slider, servo_slider_event_cb,
                        LV_EVENT_ALL, NULL);
    lv_obj_add_flag(s_actuator_card, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < UI_SENSOR_VALUE_MAX; i++) {
        s_metric_cards[i] = lv_obj_create(s_detail_content);
        lv_obj_add_style(s_metric_cards[i], &s_style_metric_card,
                         LV_PART_MAIN);
        lv_obj_set_pos(s_metric_cards[i], 0, i * 142);
        lv_obj_set_size(s_metric_cards[i], 224, 134);
        lv_obj_clear_flag(s_metric_cards[i], LV_OBJ_FLAG_SCROLLABLE);

        s_metric_names[i] = lv_label_create(s_metric_cards[i]);
        lv_obj_set_pos(s_metric_names[i], 0, 0);
        s_metric_values[i] = lv_label_create(s_metric_cards[i]);
        lv_obj_align(s_metric_values[i], LV_ALIGN_TOP_RIGHT, 0, 0);

        s_metric_charts[i] = lv_chart_create(s_metric_cards[i]);
        lv_obj_set_pos(s_metric_charts[i], 0, 25);
        lv_obj_set_size(s_metric_charts[i], 208, 94);
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
    if (state == NULL || s_sensor_count == NULL) {
        return;
    }

    int sensor_count = 0;
    int actuator_count = 0;
    for (int i = 0; i < state->device_count; i++) {
        const UiDeviceCard* device = &state->devices[i];
        const bool measurement = ui_sensor_capability_has_measurements(
            device->capability);
        const bool actuator = capability_has_buzzer(device->capability) ||
                              capability_has_servo(device->capability);
        if (measurement) sensor_count++;
        if (!measurement || actuator) actuator_count++;
    }
    lv_label_set_text_fmt(s_sensor_count, "%d device%s", sensor_count,
                          sensor_count == 1 ? "" : "s");
    lv_label_set_text_fmt(s_actuator_count, "%d device%s", actuator_count,
                          actuator_count == 1 ? "" : "s");
    lv_label_set_text(s_list_title,
                      s_category == UI_CATEGORY_SENSORS
                          ? "Sensors"
                          : "Actuators");

    int shown = 0;
    memset(s_list_module_ids, 0, sizeof(s_list_module_ids));
    for (int i = 0; i < state->device_count && shown < UI_DEVICE_MAX; i++) {
        const UiDeviceCard* card = &state->devices[i];
        const bool measurement = ui_sensor_capability_has_measurements(
            card->capability);
        const bool buzzer = capability_has_buzzer(card->capability);
        const bool servo = capability_has_servo(card->capability);
        const bool include = s_category == UI_CATEGORY_SENSORS
                                 ? measurement
                                 : (!measurement || buzzer || servo);
        if (!include) {
            continue;
        }

        const int slot = shown++;
        s_list_module_ids[slot] = card->module_id;
        set_card_style(s_cards[slot], card->connected);
        lv_label_set_text_fmt(s_name_labels[slot], "%s  #%u", card->name,
                              (unsigned)card->module_id);
        if (measurement) {
            lv_label_set_text_fmt(s_data_labels[slot], "Data: %s", card->data);
        } else if (servo && buzzer) {
            lv_label_set_text(s_data_labels[slot], "Servo / Buzzer control");
        } else if (servo) {
            lv_label_set_text(s_data_labels[slot], "Servo control");
        } else if (buzzer) {
            lv_label_set_text(s_data_labels[slot], "Buzzer control");
        } else {
            lv_label_set_text(s_data_labels[slot], "Control device");
        }
        lv_label_set_text(s_state_labels[slot],
                          card->connected ? "ONLINE" : "OFFLINE");
        lv_obj_clear_flag(s_cards[slot], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = shown; i < UI_DEVICE_MAX; i++) {
        lv_obj_add_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (shown == 0) {
        lv_obj_clear_flag(s_list_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_list_empty, LV_OBJ_FLAG_HIDDEN);
    }
    update_detail(state);
}

uint8_t ui_screen_diag_selected_module(void)
{
    return s_selected_module_id;
}

void ui_screen_diag_navigate_back(void)
{
    if (s_detail != NULL &&
        !lv_obj_has_flag(s_detail, LV_OBJ_FLAG_HIDDEN)) {
        close_detail();
    } else if (s_device_list != NULL &&
               !lv_obj_has_flag(s_device_list, LV_OBJ_FLAG_HIDDEN)) {
        show_home();
    }
}
