#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t raw_max_x;
    uint16_t raw_max_y;
    uint16_t panel_max_x;
    uint16_t panel_max_y;
    bool swap_xy;
    bool invert_x;
    bool invert_y;
} touch_transform_config_t;

typedef struct {
    bool pressed;
    uint16_t x;
    uint16_t y;
    uint8_t gesture;
} touch_decoded_sample_t;

/** Decode CST816D registers 0x01..0x06 into panel coordinates. */
bool touch_decode_registers(const uint8_t* registers, size_t register_count,
                            const touch_transform_config_t* transform,
                            touch_decoded_sample_t* out);

#ifdef __cplusplus
}
#endif
