#include "hw_drivers/pump_control.h"

#include <string.h>

static bool force_off(PumpControl* control)
{
    if (control == NULL) return false;
    bool output_off = false;
    if (control->backend.set_output != NULL) {
        output_off =
            control->backend.set_output(control->backend.context, false);
    }
    control->active = false;
    return output_off;
}

bool pump_control_init(PumpControl* control,
                       const PumpControlBackend* backend,
                       uint32_t tick_period_ms,
                       uint32_t max_run_ms)
{
    if (control == NULL || backend == NULL || backend->set_output == NULL ||
        backend->arm_timer == NULL || backend->cancel_timer == NULL ||
        tick_period_ms == 0 || max_run_ms == 0) {
        return false;
    }

    memset(control, 0, sizeof(*control));
    control->backend = *backend;
    control->tick_period_ms = tick_period_ms;
    control->max_run_ms = max_run_ms;
    if (!control->backend.set_output(control->backend.context, false)) {
        return false;
    }
    control->initialized = true;
    return true;
}

bool pump_control_apply(PumpControl* control, uint32_t duration_ms)
{
    if (control == NULL || !control->initialized) return false;

    if (duration_ms == 0) {
        bool output_off = force_off(control);
        control->backend.cancel_timer(control->backend.context);
        return output_off;
    }

    if (duration_ms > control->max_run_ms) {
        force_off(control);
        control->backend.cancel_timer(control->backend.context);
        return false;
    }

    uint32_t ticks =
        (duration_ms + control->tick_period_ms - 1) /
        control->tick_period_ms;
    if (ticks == 0) ticks = 1;
    if (!control->backend.arm_timer(control->backend.context, ticks)) {
        force_off(control);
        return false;
    }
    if (!control->backend.set_output(control->backend.context, true)) {
        control->backend.cancel_timer(control->backend.context);
        force_off(control);
        return false;
    }
    control->active = true;
    return true;
}

void pump_control_timer_fired(PumpControl* control)
{
    force_off(control);
}

bool pump_control_is_active(const PumpControl* control)
{
    return control != NULL && control->active;
}
