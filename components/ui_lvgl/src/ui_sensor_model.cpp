#include "ui_lvgl/ui_sensor_model.h"

#include <stdio.h>
#include <string.h>

static void copy_text(char* dst, size_t dst_len, const char* src)
{
    if (dst == NULL || dst_len == 0) {
        return;
    }
    snprintf(dst, dst_len, "%s", src ? src : "");
}

bool ui_sensor_capability_has_measurements(const char* capability)
{
    if (capability == NULL) {
        return false;
    }
    return strstr(capability, "remote_read") != NULL ||
           strstr(capability, "Returns ") != NULL ||
           strstr(capability, "Generic ADC") != NULL;
}

void ui_sensor_metric_meta(const char* capability, int value_index,
                           ui_metric_meta_t* out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    if (capability != NULL && strstr(capability, "DHT11") != NULL) {
        if (value_index == 0) {
            copy_text(out->label, sizeof(out->label), "Temperature");
            copy_text(out->unit, sizeof(out->unit), "C");
            return;
        }
        if (value_index == 1) {
            copy_text(out->label, sizeof(out->label), "Humidity");
            copy_text(out->unit, sizeof(out->unit), "%");
            return;
        }
    }

    if (capability != NULL && strstr(capability, "BH1750") != NULL &&
        value_index == 0) {
        copy_text(out->label, sizeof(out->label), "Illuminance");
        copy_text(out->unit, sizeof(out->unit), "lux");
        return;
    }
    if (capability != NULL && strstr(capability, "Vibration") != NULL &&
        value_index == 0) {
        copy_text(out->label, sizeof(out->label), "Vibration");
        out->binary = true;
        return;
    }
    if (capability != NULL && strstr(capability, "Raindrop") != NULL &&
        value_index == 0) {
        copy_text(out->label, sizeof(out->label), "Rain");
        out->binary = true;
        return;
    }
    if (capability != NULL &&
        (strstr(capability, "JW01") != NULL ||
         strstr(capability, "co2,tvoc,ch2o") != NULL)) {
        static const char* labels[] = {"CO2", "TVOC", "CH2O"};
        if (value_index >= 0 && value_index < 3) {
            copy_text(out->label, sizeof(out->label), labels[value_index]);
            return;
        }
    }
    if (capability != NULL && strstr(capability, "Generic ADC") != NULL) {
        static const char* labels[] = {"ADC GPIO4", "ADC GPIO5", "ADC GPIO6"};
        if (value_index >= 0 && value_index < 3) {
            copy_text(out->label, sizeof(out->label), labels[value_index]);
            return;
        }
    }

    snprintf(out->label, sizeof(out->label), "Value %d", value_index + 1);
}

void ui_sensor_history_reset(ui_sensor_history_t* history)
{
    if (history != NULL) {
        memset(history, 0, sizeof(*history));
    }
}

void ui_sensor_history_record(ui_sensor_history_t* history,
                              uint32_t timestamp_ms, const double* values,
                              int value_count)
{
    if (history == NULL || values == NULL || value_count <= 0) {
        return;
    }
    if (value_count > UI_SENSOR_VALUE_MAX) {
        value_count = UI_SENSOR_VALUE_MAX;
    }

    uint16_t index;
    if (history->count < UI_SENSOR_HISTORY_CAPACITY) {
        index = (uint16_t)((history->start + history->count) %
                           UI_SENSOR_HISTORY_CAPACITY);
        history->count++;
    } else {
        index = history->start;
        history->start =
            (uint16_t)((history->start + 1) % UI_SENSOR_HISTORY_CAPACITY);
    }

    ui_sensor_history_sample_t* sample = &history->samples[index];
    memset(sample, 0, sizeof(*sample));
    sample->timestamp_ms = timestamp_ms;
    sample->value_count = (uint8_t)value_count;
    for (int i = 0; i < value_count; i++) {
        sample->values[i] = (float)values[i];
    }
}

int ui_sensor_history_copy_metric(const ui_sensor_history_t* history,
                                  int value_index, uint32_t now_ms,
                                  uint32_t window_ms, float* out,
                                  int max_values)
{
    if (history == NULL || value_index < 0 ||
        value_index >= UI_SENSOR_VALUE_MAX || out == NULL || max_values <= 0) {
        return 0;
    }

    int copied = 0;
    for (uint16_t i = 0; i < history->count && copied < max_values; i++) {
        const uint16_t index =
            (uint16_t)((history->start + i) % UI_SENSOR_HISTORY_CAPACITY);
        const ui_sensor_history_sample_t* sample = &history->samples[index];
        if (value_index >= sample->value_count) {
            continue;
        }
        const uint32_t age_ms = now_ms - sample->timestamp_ms;
        if (age_ms <= window_ms) {
            out[copied++] = sample->values[value_index];
        }
    }
    return copied;
}

int ui_sensor_find_module_index(const uint8_t* module_ids, int count,
                                uint8_t selected_module_id)
{
    if (module_ids == NULL || count <= 0 || selected_module_id == 0) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (module_ids[i] == selected_module_id) {
            return i;
        }
    }
    return -1;
}
