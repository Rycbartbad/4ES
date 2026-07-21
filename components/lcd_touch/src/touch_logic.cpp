#include "lcd_touch/touch_logic.h"

static uint16_t scale_coordinate(uint16_t raw, uint16_t raw_max,
                                 uint16_t panel_max)
{
    if (raw > raw_max) {
        raw = raw_max;
    }
    return (uint16_t)(((uint32_t)raw * panel_max) / raw_max);
}

bool touch_decode_registers(const uint8_t* registers, size_t register_count,
                            const touch_transform_config_t* transform,
                            touch_decoded_sample_t* out)
{
    if (registers == NULL || register_count < 6 || transform == NULL ||
        out == NULL || transform->raw_max_x == 0 ||
        transform->raw_max_y == 0 || transform->panel_max_x == 0 ||
        transform->panel_max_y == 0) {
        return false;
    }

    const uint16_t raw_x =
        (uint16_t)(((uint16_t)(registers[2] & 0x0F) << 8) | registers[3]);
    const uint16_t raw_y =
        (uint16_t)(((uint16_t)(registers[4] & 0x0F) << 8) | registers[5]);

    uint16_t x = scale_coordinate(raw_x, transform->raw_max_x,
                                  transform->panel_max_x);
    uint16_t y = scale_coordinate(raw_y, transform->raw_max_y,
                                  transform->panel_max_y);
    if (transform->swap_xy) {
        const uint16_t tmp = x;
        x = y;
        y = tmp;
    }
    if (transform->invert_x) {
        x = transform->panel_max_x - x;
    }
    if (transform->invert_y) {
        y = transform->panel_max_y - y;
    }

    out->gesture = registers[0];
    out->pressed = registers[1] > 0;
    out->x = x;
    out->y = y;
    return true;
}
