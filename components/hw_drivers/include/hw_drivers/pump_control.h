#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*pump_set_output_fn)(void* context, bool active);
typedef bool (*pump_arm_timer_fn)(void* context, uint32_t delay_ticks);
typedef void (*pump_cancel_timer_fn)(void* context);

typedef struct {
    void* context;
    pump_set_output_fn set_output;
    pump_arm_timer_fn arm_timer;
    pump_cancel_timer_fn cancel_timer;
} PumpControlBackend;

typedef struct {
    PumpControlBackend backend;
    uint32_t tick_period_ms;
    uint32_t max_run_ms;
    bool initialized;
    bool active;
} PumpControl;

bool pump_control_init(PumpControl* control,
                       const PumpControlBackend* backend,
                       uint32_t tick_period_ms,
                       uint32_t max_run_ms);
bool pump_control_apply(PumpControl* control, uint32_t duration_ms);
void pump_control_timer_fired(PumpControl* control);
bool pump_control_is_active(const PumpControl* control);

#ifdef __cplusplus
}
#endif
