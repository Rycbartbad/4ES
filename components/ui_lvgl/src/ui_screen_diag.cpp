#include "sdkconfig.h"

#include "lvgl.h"

#include "ui_lvgl_internal.h"

static lv_obj_t* s_count_label = NULL;
static lv_obj_t* s_cards[UI_SENSOR_CARD_MAX] = {};
static lv_obj_t* s_name_labels[UI_SENSOR_CARD_MAX] = {};
static lv_obj_t* s_data_labels[UI_SENSOR_CARD_MAX] = {};
static lv_obj_t* s_state_labels[UI_SENSOR_CARD_MAX] = {};

static lv_style_t s_style_screen;
static lv_style_t s_style_title;
static lv_style_t s_style_card_online;
static lv_style_t s_style_card_offline;
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

void ui_screen_diag_create(UiStatusState* state)
{
    (void)state;

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

    lv_style_init(&s_style_name);
    lv_style_set_text_color(&s_style_name, lv_color_hex(0xFFFFFF));

    lv_style_init(&s_style_data);
    lv_style_set_text_color(&s_style_data, lv_color_hex(0xFFFFFF));

    lv_style_init(&s_style_state);
    lv_style_set_text_color(&s_style_state, lv_color_hex(0xFFFFFF));

    lv_obj_t* scr = lv_scr_act();
    lv_obj_add_style(scr, &s_style_screen, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(scr);
    lv_obj_add_style(title, &s_style_title, LV_PART_MAIN);
    lv_label_set_text(title, "ESP-LEGO Sensors");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 6, 4);

    s_count_label = lv_label_create(scr);
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
        int col = i % 2;
        int row = i / 2;
        lv_coord_t x = margin + col * (card_w + gap);
        lv_coord_t y = top + row * (card_h + gap);

        s_cards[i] = lv_obj_create(scr);
        lv_obj_set_pos(s_cards[i], x, y);
        lv_obj_set_size(s_cards[i], card_w, card_h);
        lv_obj_clear_flag(s_cards[i], LV_OBJ_FLAG_SCROLLABLE);
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
    }
}
