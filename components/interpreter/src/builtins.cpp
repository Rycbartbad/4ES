/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO V1.0 — Built-in functions (P6)
 *
 * 28 builtins registered into the global environment:
 *
 *   GPIO/ADC/PWM:      digital_read, digital_write, analog_read, analog_write
 *   Timing:            sleep
 *   I/O:               print
 *   ESP-NOW peer mgr:  list_peers, peer_count, peer_online
 *   ESP-NOW comm:      remote_read, espnow_send
 *   Remote buzzer:     buzzer_beep, buzzer_note, buzzer_song
 *   Remote servo:      servo_write, servo_sweep
 *   Remote pump:       pump_write
 *   List ops:          list_new, list_get, list_set, list_len, list_free
 *   Aggregation:       remote_read_avg, remote_read_max, remote_read_min
 *   Cached remote:     read_sensor (LCD-synced), send_motor, mic_level
 */

#include "sdkconfig.h"

#include "interpreter/builtins.h"
#include "interpreter/interpreter.h"
#include "interpreter/value.h"
#include "interpreter/intern.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hw_drivers/drivers.h"

#include "espnow_comm/comm.h"
#include "espnow_comm/peer_mgr.h"

#include "esp_log.h"

static const char* TAG = "builtins";

#define BUZZER_NOTE_G5  19
#define BUZZER_DEFAULT_MS 200

// ====================================================================
// Value construction helpers (C++17-safe — no designated initializers)
// ====================================================================

static inline Value bval_undefined(void)
{
    Value v;
    v.type = VAL_UNDEFINED;
    v.num  = 0;
    return v;
}

static inline Value bval_num(double n)
{
    Value v;
    v.type = VAL_NUM;
    v.num  = n;
    return v;
}

static inline Value bval_bool(bool b)
{
    Value v;
    v.type = VAL_BOOL;
    v.b    = b;
    return v;
}

static inline Value bval_list(ListData* lst)
{
    Value v;
    v.type = VAL_LIST;
    v.list = lst;
    return v;
}

static inline Value bval_str(const char* s)
{
    Value v;
    v.type = VAL_STR;
    v.str  = s;
    return v;
}

// ====================================================================
// Builtin function type and registration table
// ====================================================================

typedef struct {
    const char* name;
    int         param_count;
    ControlCommandHandler func;
} BuiltinEntry;

// ---- Forward declarations of all builtin implementations -------------

static Value bif_digital_read(Value* args, int n, ExecutionContext* ctx);
static Value bif_digital_write(Value* args, int n, ExecutionContext* ctx);
static Value bif_analog_read(Value* args, int n, ExecutionContext* ctx);
static Value bif_analog_write(Value* args, int n, ExecutionContext* ctx);
static Value bif_sleep(Value* args, int n, ExecutionContext* ctx);
static Value bif_print(Value* args, int n, ExecutionContext* ctx);
static Value bif_list_peers(Value* args, int n, ExecutionContext* ctx);
static Value bif_peer_count(Value* args, int n, ExecutionContext* ctx);
static Value bif_peer_online(Value* args, int n, ExecutionContext* ctx);
static Value bif_remote_read(Value* args, int n, ExecutionContext* ctx);
static Value bif_espnow_send(Value* args, int n, ExecutionContext* ctx);
static Value bif_list_new(Value* args, int n, ExecutionContext* ctx);
static Value bif_list_get(Value* args, int n, ExecutionContext* ctx);
static Value bif_list_set(Value* args, int n, ExecutionContext* ctx);
static Value bif_list_len(Value* args, int n, ExecutionContext* ctx);
static Value bif_remote_read_avg(Value* args, int n, ExecutionContext* ctx);
static Value bif_remote_read_max(Value* args, int n, ExecutionContext* ctx);
static Value bif_remote_read_min(Value* args, int n, ExecutionContext* ctx);
static Value bif_list_free_builtin(Value* args, int n, ExecutionContext* ctx);
static Value bif_read_sensor(Value* args, int n, ExecutionContext* ctx);
static Value bif_send_motor(Value* args, int n, ExecutionContext* ctx);
static Value bif_mic_level(Value* args, int n, ExecutionContext* ctx);
static Value bif_buzzer_beep(Value* args, int n, ExecutionContext* ctx);
static Value bif_buzzer_note(Value* args, int n, ExecutionContext* ctx);
static Value bif_buzzer_song(Value* args, int n, ExecutionContext* ctx);
static Value bif_servo_write(Value* args, int n, ExecutionContext* ctx);
static Value bif_servo_sweep(Value* args, int n, ExecutionContext* ctx);
static Value bif_pump_write(Value* args, int n, ExecutionContext* ctx);

// ---- Registration table ----------------------------------------------

static const BuiltinEntry s_builtin_entries[] = {
    {"digital_read",     1, bif_digital_read},
    {"digital_write",    2, bif_digital_write},
    {"analog_read",      1, bif_analog_read},
    {"analog_write",     2, bif_analog_write},
    {"sleep",            1, bif_sleep},
    {"print",            1, bif_print},
    {"list_peers",       0, bif_list_peers},
    {"peer_count",       0, bif_peer_count},
    {"peer_online",      1, bif_peer_online},
    {"remote_read",      2, bif_remote_read},
    {"espnow_send",      3, bif_espnow_send},
    {"list_new",         1, bif_list_new},
    {"list_get",         2, bif_list_get},
    {"list_set",         3, bif_list_set},
    {"list_len",         1, bif_list_len},
    {"remote_read_avg",  1, bif_remote_read_avg},
    {"remote_read_max",  1, bif_remote_read_max},
    {"remote_read_min",  1, bif_remote_read_min},
    {"list_free",        1, bif_list_free_builtin},
    {"send_motor",       2, bif_send_motor},
    {"mic_level",        0, bif_mic_level},
};

static const ControlArgSpec kReadSensorArgs[] = {
    {"device", CONTROL_ARG_DEVICE, 0, 0,
     "stable sensor name or type word"},
};
static const ControlArgSpec kBuzzerBeepArgs[] = {
    {"device", CONTROL_ARG_DEVICE, 0, 0,
     "stable buzzer name or type word"},
    {"count", CONTROL_ARG_NUMBER, 1, 20, "number of beeps"},
};
static const ControlArgSpec kBuzzerNoteArgs[] = {
    {"device", CONTROL_ARG_DEVICE, 0, 0,
     "stable buzzer name or type word"},
    {"note", CONTROL_ARG_NUMBER, 0, 36, "note number"},
    {"dur", CONTROL_ARG_NUMBER, 1, 30000, "duration in milliseconds"},
};
static const ControlArgSpec kBuzzerSongArgs[] = {
    {"device", CONTROL_ARG_DEVICE, 0, 0,
     "stable buzzer name or type word"},
    {"song", CONTROL_ARG_NUMBER, 0, 2, "preset song number"},
};
static const ControlArgSpec kServoWriteArgs[] = {
    {"device", CONTROL_ARG_DEVICE, 0, 0,
     "stable servo name or type word"},
    {"angle", CONTROL_ARG_NUMBER, 0, 180, "angle in degrees"},
};
static const ControlArgSpec kServoSweepArgs[] = {
    {"device", CONTROL_ARG_DEVICE, 0, 0,
     "stable servo name or type word"},
    {"from", CONTROL_ARG_NUMBER, 0, 180, "starting angle"},
    {"to", CONTROL_ARG_NUMBER, 0, 180, "ending angle"},
    {"step", CONTROL_ARG_NUMBER, 1, 180, "angle step"},
    {"delay", CONTROL_ARG_NUMBER, 1, 30000,
     "delay between steps in milliseconds"},
};
static const ControlArgSpec kPumpWriteArgs[] = {
    {"device", CONTROL_ARG_DEVICE, 0, 0,
     "stable pump name or type word"},
    {"duration_ms", CONTROL_ARG_NUMBER, 0, 30000,
     "0 turns off; 1..30000 runs for a finite duration"},
};

static const ControlCommandSpec s_control_commands[] = {
    {"read_sensor", "sensor", "Read and print a cached sensor value.",
     kReadSensorArgs, 1, true, bif_read_sensor},
    {"buzzer_beep", "buzzer", "Beep a buzzer a finite number of times.",
     kBuzzerBeepArgs, 2, false, bif_buzzer_beep},
    {"buzzer_note", "buzzer", "Play one buzzer note for a finite duration.",
     kBuzzerNoteArgs, 3, false, bif_buzzer_note},
    {"buzzer_song", "buzzer", "Play a preset finite buzzer song.",
     kBuzzerSongArgs, 2, false, bif_buzzer_song},
    {"servo_write", "servo", "Set a servo angle from 0 to 180 degrees.",
     kServoWriteArgs, 2, false, bif_servo_write},
    {"servo_sweep", "servo", "Sweep a servo through a finite angle range.",
     kServoSweepArgs, 5, false, bif_servo_sweep},
    {"pump_write", "pump",
     "Turn a pump off or run it for at most 30000 milliseconds.",
     kPumpWriteArgs, 2, false, bif_pump_write},
};

static_assert(sizeof(s_builtin_entries) / sizeof(s_builtin_entries[0]) == 21,
              "BIF_COUNT base builtin count mismatch");
static_assert(sizeof(s_control_commands) / sizeof(s_control_commands[0]) ==
                  CONTROL_COMMAND_COUNT,
              "CONTROL_COMMAND_COUNT mismatch");
static_assert(21 + CONTROL_COMMAND_COUNT == BIF_COUNT,
              "BIF_COUNT must include control commands");

const ControlCommandSpec* control_command_specs(size_t* out_count)
{
    if (out_count != NULL) {
        *out_count = sizeof(s_control_commands) /
                     sizeof(s_control_commands[0]);
    }
    return s_control_commands;
}

const ControlCommandSpec* control_command_find(const char* dsl_name)
{
    if (dsl_name == NULL) return NULL;
    for (size_t i = 0;
         i < sizeof(s_control_commands) / sizeof(s_control_commands[0]); i++) {
        if (strcmp(s_control_commands[i].dsl_name, dsl_name) == 0) {
            return &s_control_commands[i];
        }
    }
    return NULL;
}

int control_command_format_dsl(const char* dsl_name,
                               const char* const* arg_literals,
                               size_t arg_count,
                               char* out, size_t out_len)
{
    const ControlCommandSpec* spec = control_command_find(dsl_name);
    if (spec == NULL || arg_literals == NULL || out == NULL || out_len == 0 ||
        arg_count != spec->arg_count) {
        return -1;
    }

    size_t pos = 0;
    int w = snprintf(out, out_len, "%s%s(", spec->print_result ? "print(" : "",
                     spec->dsl_name);
    if (w < 0 || (size_t)w >= out_len) return -1;
    pos = (size_t)w;
    for (size_t i = 0; i < arg_count; i++) {
        if (arg_literals[i] == NULL) return -1;
        w = snprintf(out + pos, out_len - pos, "%s%s",
                     i ? "," : "", arg_literals[i]);
        if (w < 0 || (size_t)w >= out_len - pos) return -1;
        pos += (size_t)w;
    }
    w = snprintf(out + pos, out_len - pos,
                 spec->print_result ? "));\n" : ");\n");
    if (w < 0 || (size_t)w >= out_len - pos) return -1;
    pos += (size_t)w;
    return (int)pos;
}

// ---- Persistent FuncObj array for builtins (body = NULL = builtin) ---

// Callback: read cached remote sensor values (LCD sync).
// Set by ui_lvgl_init(); remains NULL if LCD is not compiled in.
// Signature: (module_id, out_values, max) → count of values copied.
extern int (*g_sensor_read_callback)(uint8_t module_id, double* out, int max);
int (*g_sensor_read_callback)(uint8_t module_id, double* out, int max) = NULL;

static FuncObj s_builtin_funcs[BIF_COUNT];
static bool    s_builtins_registered = false;

// ====================================================================
// register_builtins — called once at startup
// ====================================================================

void register_builtins(Environment* env)
{
    if (s_builtins_registered) return;
    s_builtins_registered = true;

    int index = 0;
    for (size_t i = 0;
         i < sizeof(s_builtin_entries) / sizeof(s_builtin_entries[0]);
         i++, index++) {
        FuncObj* fo = &s_builtin_funcs[index];
        fo->name = intern_string(s_builtin_entries[i].name,
                                 (int)strlen(s_builtin_entries[i].name));
        fo->param_count = s_builtin_entries[i].param_count;
        fo->body        = NULL;             // signals builtin to interpreter

        Value v;
        v.type = VAL_FUNC;
        v.func = fo;
        env_define(env, s_builtin_entries[i].name, v);
        // Note: env_define stores the .rodata string literal pointer, NOT
        // an interned pointer.  env_get uses pointer equality, so the
        // caller (call_function) must fall through to call_builtin_by_name
        // strcmp dispatch rather than relying on pointer match.
        // This is intentional — see design.md §6.9.1 for reasoning.
    }
    for (size_t i = 0;
         i < sizeof(s_control_commands) / sizeof(s_control_commands[0]);
         i++, index++) {
        FuncObj* fo = &s_builtin_funcs[index];
        fo->name = intern_string(s_control_commands[i].dsl_name,
                                 (int)strlen(s_control_commands[i].dsl_name));
        fo->param_count = s_control_commands[i].arg_count;
        fo->body = NULL;

        Value v;
        v.type = VAL_FUNC;
        v.func = fo;
        env_define(env, s_control_commands[i].dsl_name, v);
    }

    // Register print callback capture (web_console integration)
    g_print_callback = NULL;  // reset; web_console sets it later
}

// ====================================================================
// Optional print callback (set by web_console)
// ====================================================================

void (*g_print_callback)(const char* str, int len) = NULL;

// ====================================================================
// Helper: consistent argument validation for builtins
// Returns true if arg_count >= required, logs warning otherwise.
// ====================================================================
static bool check_args(const char* func, int arg_count, int required)
{
    if (arg_count < required) {
        ESP_LOGW(TAG, "%s() requires %d args, got %d", func, required, arg_count);
        return false;
    }
    return true;
}

// Surface a fuzzy device resolution to BOTH the ESP log and the Web Console
// execution log, so the user can see which physical device a vague reference
// ("舵机", "温度", ...) was mapped to — especially when several matched.
static void report_type_resolution(const char* func, const char* query,
                                   const char* type, const PeerEntry* tp,
                                   int match_count)
{
    char line[128];
    int len;
    if (match_count > 1) {
        len = snprintf(line, sizeof(line),
            "[%s: \"%s\" -> %s (id=%u); %d '%s' devices online, using this one]\n",
            func, query, tp->name, tp->module_id, match_count, type);
        ESP_LOGW(TAG, "%s(\"%s\"): %d '%s' devices online, using id=%u (%s)",
                 func, query, match_count, type, tp->module_id, tp->name);
    } else {
        len = snprintf(line, sizeof(line),
            "[%s: \"%s\" -> %s (id=%u)]\n",
            func, query, tp->name, tp->module_id);
        ESP_LOGI(TAG, "%s(\"%s\") resolved by type '%s' -> id=%u (%s)",
                 func, query, type, tp->module_id, tp->name);
    }
    if (len > 0 && g_print_callback != NULL) {
        g_print_callback(line, len);
    }
}

static bool resolve_module_id_arg(const char* func,
                                  const Value* arg,
                                  ExecutionContext* ctx,
                                  uint8_t* module_id_out)
{
    if (!arg || !module_id_out) return false;

    if (arg->type == VAL_NUM) {
        if (!isfinite(arg->num) || arg->num < 0 || arg->num > 255 ||
            floor(arg->num) != arg->num) {
            ESP_LOGW(TAG, "%s(): module id must be an integer from 0 to 255",
                     func);
            return false;
        }
        *module_id_out = (uint8_t)arg->num;
        return true;
    }

    if (arg->type == VAL_STR && arg->str) {
        bool conflict = false;
        PeerEntry* peer = peer_mgr_find_by_name(arg->str, &conflict);
        if (conflict) {
            ESP_LOGW(TAG, "%s(\"%s\"): name conflict detected", func, arg->str);
#if CONFIG_STRICT_MODE
            if (ctx) {
                ctx->constraint_violated = true;
                ctx->violation_msg       = "Peer name conflict detected";
            }
            return false;
#endif
        }
        if (peer) {
            *module_id_out = peer->module_id;
            return true;
        }

        // Fuzzy fallback: the string is not an exact peer name, so treat it
        // as a device TYPE / synonym ("舵机", "servo", "门铃", "温度", ...)
        // and resolve to the matching online peer. This makes vague, id-less
        // references work even when the model emits a type word as the id.
        const char* type = peer_mgr_type_from_query(arg->str);
        if (type) {
            int match_count = 0;
            PeerEntry* tp = peer_mgr_find_by_type(type, &match_count);

            // A specific sensor sub-type ("温度"/"light"/...) may have no
            // dedicated device online — fall back to any generic sensor so a
            // single-sensor setup still responds.
            if (tp == NULL &&
                strcmp(type, "servo") != 0 &&
                strcmp(type, "buzzer") != 0 &&
                strcmp(type, "pump") != 0 &&
                strcmp(type, "sensor") != 0) {
                tp = peer_mgr_find_by_type("sensor", &match_count);
            }

            if (tp) {
                report_type_resolution(func, arg->str, type, tp, match_count);
                *module_id_out = tp->module_id;
                return true;
            }
        }
    }

    ESP_LOGW(TAG, "%s(): invalid or unknown module id", func);
    return false;
}

static bool validate_control_device_type(const ControlCommandSpec* spec,
                                         const Value* args,
                                         int arg_count,
                                         ExecutionContext* ctx)
{
    if (!spec || !args || arg_count < 1 || spec->arg_count < 1 ||
        spec->args[0].kind != CONTROL_ARG_DEVICE) {
        return true;
    }

    uint8_t module_id = 0;
    if (!resolve_module_id_arg(spec->dsl_name, &args[0], ctx, &module_id)) {
        return false;
    }

    bool conflict = false;
    PeerEntry* peer = peer_mgr_find_by_id(module_id, &conflict);
    if (conflict || (peer && !peer_mgr_matches_type(peer, spec->device_type))) {
        ESP_LOGW(TAG,
                 "%s(): device id=%u is not an unambiguous %s (actual=%s)",
                 spec->dsl_name, module_id, spec->device_type,
                 peer ? peer->name : "conflict");
        if (ctx) {
            ctx->constraint_violated = true;
            ctx->violation_msg = "Control command device type mismatch";
        }
        return false;
    }

    return true;
}

// ====================================================================
// call_builtin_by_name — dispatched by interpreter.cpp
// ====================================================================

Value call_builtin_by_name(const char* name, const Value* args,
                           int arg_count, ExecutionContext* ctx)
{
    // We need a mutable copy for builtins that may need to modify args
    // (though none currently do; this is defensive).
    // Build a local args array of at-most-16 elements.
    Value local_args[16];
    int n = (arg_count < 16) ? arg_count : 16;
    for (int i = 0; i < n; i++) {
        local_args[i] = args[i];
    }

    for (size_t i = 0;
         i < sizeof(s_builtin_entries) / sizeof(s_builtin_entries[0]); i++) {
        if (strcmp(name, s_builtin_entries[i].name) == 0) {
            return s_builtin_entries[i].func(local_args, n, ctx);
        }
    }
    for (size_t i = 0;
         i < sizeof(s_control_commands) / sizeof(s_control_commands[0]); i++) {
        if (strcmp(name, s_control_commands[i].dsl_name) == 0) {
            if (!validate_control_device_type(&s_control_commands[i],
                                              local_args, n, ctx)) {
                return bval_num(-1);
            }
            return s_control_commands[i].handler(local_args, n, ctx);
        }
    }

    // Not found — signal constraint violation
    ctx->constraint_violated = true;
    ctx->violation_msg       = "Undefined function";
    return bval_undefined();
}

// ====================================================================
// Helper — check sensor call limit, returns true if violated
// ====================================================================

static bool sensor_call_check(ExecutionContext* ctx)
{
    ctx->sensor_calls_total++;
    if (ctx->sensor_calls_total > CONFIG_MAX_SENSOR_CALLS_PER_SCRIPT) {
        if (CONFIG_STRICT_MODE) {
            ctx->constraint_violated = true;
            ctx->violation_msg       = "Sensor call limit exceeded";
        }
        return true;
    }
    return false;
}

// ====================================================================
// Helper — parse argument list as module IDs into a uint8_t array.
// Accepts either a single comma-separated string, or multiple number args.
// Returns number of IDs parsed (0 on error / empty).
// ====================================================================

#define MAX_PARSE_IDS 16

static int parse_module_ids(Value* args, int arg_count,
                            uint8_t* ids_out, int ids_max)
{
    int id_count = 0;

    // Single string arg — parse comma-separated IDs
    if (arg_count == 1 && args[0].type == VAL_STR && args[0].str) {
        char buf[128];
        strncpy(buf, args[0].str, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char* saveptr;
        char* token = strtok_r(buf, ",", &saveptr);
        while (token && id_count < ids_max) {
            // Trim leading whitespace
            while (*token == ' ' || *token == '\t') token++;
            if (*token >= '0' && *token <= '9') {
                ids_out[id_count++] = (uint8_t)atoi(token);
            }
            token = strtok_r(NULL, ",", &saveptr);
        }
        return id_count;
    }

    // Multiple number args
    for (int i = 0; i < arg_count && id_count < ids_max; i++) {
        if (args[i].type == VAL_NUM) {
            ids_out[id_count++] = (uint8_t)args[i].num;
        }
    }
    return id_count;
}

// ====================================================================
// 1. digital_read(pin) — GPIO digital input
// ====================================================================

static Value bif_digital_read(Value* args, int n, ExecutionContext* ctx)
{
    (void)ctx;
    if (n < 1 || args[0].type != VAL_NUM) return bval_num(0);
    return bval_num((double)hw_gpio_read((uint8_t)args[0].num));
}

// ====================================================================
// 2. digital_write(pin, val) — GPIO digital output
// ====================================================================

static Value bif_digital_write(Value* args, int n, ExecutionContext* ctx)
{
    (void)ctx;
    if (n >= 2 && args[0].type == VAL_NUM) {
        uint8_t pin = (uint8_t)args[0].num;
        hw_gpio_write(pin, (int)args[1].num);
        track_output_pin(pin);
    }
    return bval_undefined();
}

// ====================================================================
// 3. analog_read(pin) — ADC input
// ====================================================================

static Value bif_analog_read(Value* args, int n, ExecutionContext* ctx)
{
    (void)ctx;
    if (n < 1 || args[0].type != VAL_NUM) return bval_num(0);
    return bval_num((double)hw_adc_read((uint8_t)args[0].num));
}

// ====================================================================
// 4. analog_write(pin, val) — PWM output
// ====================================================================

static Value bif_analog_write(Value* args, int n, ExecutionContext* ctx)
{
    (void)ctx;
    if (n >= 2 && args[0].type == VAL_NUM) {
        uint8_t pin = (uint8_t)args[0].num;
        hw_pwm_write(pin, (int)args[1].num);
        track_output_pin(pin);
    }
    return bval_undefined();
}

// ====================================================================
// 5. sleep(ms) — block via vTaskDelay
// ====================================================================

static Value bif_sleep(Value* args, int n, ExecutionContext* ctx)
{
    if (n >= 1 && args[0].type == VAL_NUM) {
        double requested_ms = args[0].num;
        if (requested_ms > 0) {
            // Delay in short slices so watchdog expiry and a replacement
            // script can interrupt sleep(). Clamp before converting the
            // untrusted numeric value to an integer.
            uint32_t remaining_ms =
                requested_ms > (double)UINT32_MAX
                    ? UINT32_MAX
                    : (uint32_t)requested_ms;
            while (remaining_ms > 0) {
                __sync_synchronize();
                if (ctx && ctx->s_script_abort_ptr &&
                    *ctx->s_script_abort_ptr) {
                    break;
                }
                if (ctx && ctx->s_script_timeout_ptr &&
                    *ctx->s_script_timeout_ptr) {
                    ctx->constraint_violated = true;
                    ctx->violation_msg = "Script execution timeout";
                    break;
                }

                const uint32_t slice_ms =
                    remaining_ms > 50U ? 50U : remaining_ms;
                TickType_t delay_ticks = pdMS_TO_TICKS(slice_ms);
                if (delay_ticks == 0) delay_ticks = 1;
                vTaskDelay(delay_ticks);
                remaining_ms -= slice_ms;
            }
        }
    }
    return bval_undefined();
}

// ====================================================================
// 6. print(val) — formatted output to stdout
// ====================================================================

static Value bif_print(Value* args, int n, ExecutionContext* ctx)
{
    (void)ctx;
    if (n >= 1) {
        const Value* v = &args[0];
        char line[256];
        int len = 0;

        switch (v->type) {
        case VAL_NUM: {
            double intpart;
            if (modf(v->num, &intpart) == 0.0 &&
                v->num < 1e10 && v->num > -1e10) {
                len = snprintf(line, sizeof(line), "%.0f\n", v->num);
                printf("%s", line);
            } else {
                len = snprintf(line, sizeof(line), "%g\n", v->num);
                printf("%s", line);
            }
            break;
        }
        case VAL_STR:
            len = snprintf(line, sizeof(line), "%s\n",
                           v->str ? v->str : "(null)");
            printf("%s", line);
            break;
        case VAL_BOOL:
            len = snprintf(line, sizeof(line), "%s\n",
                           v->b ? "true" : "false");
            printf("%s", line);
            break;
        case VAL_LIST:
            len = snprintf(line, sizeof(line), "[list len=%d]\n",
                           v->list ? v->list->len : 0);
            printf("%s", line);
            break;
        case VAL_FUNC:
            len = snprintf(line, sizeof(line), "[func %s]\n",
                           v->func && v->func->name
                               ? v->func->name
                               : "(anon)");
            printf("%s", line);
            break;
        default:
            len = snprintf(line, sizeof(line), "undefined\n");
            printf("%s", line);
            break;
        }
        fflush(stdout);

        // Write to Web Console ring buffer via callback (NULL-safe)
        if (len > 0 && g_print_callback != NULL) {
            g_print_callback(line, len);
        }
    }
    return bval_undefined();
}

// ====================================================================
// 7. list_peers() — formatted peer list string
// ====================================================================

static Value bif_list_peers(Value* args, int n, ExecutionContext* ctx)
{
    (void)args;
    (void)n;
    (void)ctx;

    int count = 0;
    PeerEntry** list = peer_mgr_list(&count);
    if (count == 0 || !list) {
        return bval_str(intern_string("", 0));
    }

    char buf[512];
    int pos = 0;
    for (int i = 0; i < count && pos < (int)sizeof(buf) - 32; i++) {
        int written = snprintf(buf + pos, sizeof(buf) - pos,
                               "id=%u name=%s state=%d\n",
                               list[i]->module_id,
                               list[i]->name,
                               (int)list[i]->state);
        if (written > 0) pos += written;
    }
    buf[pos] = '\0';

    return bval_str(intern_string(buf, pos));
}

// ====================================================================
// 8. peer_count() — number of active peers
// ====================================================================

static Value bif_peer_count(Value* args, int n, ExecutionContext* ctx)
{
    (void)args;
    (void)n;
    (void)ctx;
    return bval_num((double)peer_mgr_active_count());
}

// ====================================================================
// 9. peer_online(name) — check if peer is ACTIVE by name
// ====================================================================

static Value bif_peer_online(Value* args, int n, ExecutionContext* ctx)
{
    if (n < 1) return bval_bool(false);

    bool online = false;

    if (args[0].type == VAL_NUM) {
        PeerEntry* p = peer_mgr_find_by_id((uint8_t)args[0].num, NULL);
        online = (p != NULL && p->state == PEER_ACTIVE);
    } else if (args[0].type == VAL_STR && args[0].str) {
        bool conflict = false;
        PeerEntry* p = peer_mgr_find_by_name(args[0].str, &conflict);
        if (conflict) {
            ESP_LOGW(TAG, "peer_online(\"%s\"): name conflict detected", args[0].str);
#if CONFIG_STRICT_MODE
            ctx->constraint_violated = true;
            ctx->violation_msg       = "Peer name conflict detected";
            return bval_bool(false);
#endif
        }
        online = (p != NULL && p->state == PEER_ACTIVE);
    }

    return bval_bool(online);
}

// ====================================================================
// 10. remote_read(module_id) — synchronous ESP-NOW multi-sensor read
// ====================================================================

static Value bif_remote_read(Value* args, int n, ExecutionContext* ctx)
{
    if (n < 1) return bval_num(0);

    // Check sensor call limit
    if (sensor_call_check(ctx)) return bval_num(0);

    uint8_t module_id = 0;
    bool    found     = false;

    if (args[0].type == VAL_NUM) {
        module_id = (uint8_t)args[0].num;
        found     = true;
    } else if (args[0].type == VAL_STR && args[0].str) {
        bool conflict = false;
        PeerEntry* peer = peer_mgr_find_by_name(args[0].str, &conflict);
        if (conflict) {
            ESP_LOGW(TAG, "remote_read(\"%s\"): name conflict detected", args[0].str);
#if CONFIG_STRICT_MODE
            ctx->constraint_violated = true;
            ctx->violation_msg       = "Peer name conflict detected";
            return bval_num(0);
#endif
        }
        if (peer) {
            module_id = peer->module_id;
            found     = true;
        }
    }

    if (!found) return bval_num(0);

    // Fetch all sensor values from the remote module
    double values[DATA_RESP_MAX_VALUES];
    int count = espnow_comm_request_read(module_id, values, DATA_RESP_MAX_VALUES);

    if (count <= 0) return bval_num(0.0);

    // Single value → return as number (backward compatible)
    if (count == 1) return bval_num(values[0]);

    // Multiple values → return as a list
    ListData* lst = bif_pool_alloc();
    if (!lst) {
        ctx->constraint_violated = true;
        ctx->violation_msg       = "List pool exhausted";
        return bval_num(0.0);
    }

    lst->len = (count > 16) ? 16 : count;
    for (int i = 0; i < lst->len; i++) {
        lst->data[i] = values[i];
    }
    return bval_list(lst);
}

// ====================================================================
// 11. espnow_send(module_id, cmd_id [, payload...]) — send arbitrary command
// ====================================================================

static Value bif_espnow_send(Value* args, int n, ExecutionContext* ctx)
{
    (void)ctx;
    if (!check_args("espnow_send", n, 2) || args[1].type != VAL_NUM) {
        return bval_num(-1);
    }

    uint8_t module_id = 0;
    if (!resolve_module_id_arg("espnow_send", &args[0], ctx, &module_id)) {
        return bval_num(-1);
    }

    uint16_t cmd_id    = (uint16_t)args[1].num;

    // Detect module_id conflicts
    bool conflict = false;
    peer_mgr_find_by_id(module_id, &conflict);
    if (conflict) {
        ESP_LOGW(TAG, "espnow_send(module_id=%u): multiple peers share this module_id", module_id);
    }

    // Build payload from remaining args (max 32 bytes)
    uint8_t payload[32];
    int     payload_len = 0;

    for (int i = 2; i < n && payload_len < 32; i++) {
        if (args[i].type == VAL_NUM) {
            payload[payload_len++] = (uint8_t)args[i].num;
        }
    }

    esp_err_t err = espnow_comm_send_cmd(module_id, cmd_id,
                                          payload, (uint8_t)payload_len);
    return bval_num((double)err);
}

// ====================================================================
// 12. list_new(size) — allocate a new list
// ====================================================================

static Value bif_list_new(Value* args, int n, ExecutionContext* ctx)
{
    if (n < 1 || args[0].type != VAL_NUM) return bval_list(NULL);

    ListData* lst = bif_pool_alloc();
    if (!lst) {
        ctx->constraint_violated = true;
        ctx->violation_msg       = "List pool exhausted";
        return bval_list(NULL);
    }

    int size = (int)args[0].num;
    if (size < 0) size = 0;
    if (size > 16) size = 16;

    lst->len = size;
    // data[] is already zeroed by bif_pool_alloc
    return bval_list(lst);
}

// ====================================================================
// 13. list_get(list, index) — read list element by index
// ====================================================================

static Value bif_list_get(Value* args, int n, ExecutionContext* ctx)
{
    (void)ctx;
    if (n < 2 || args[0].type != VAL_LIST || !args[0].list) {
        return bval_num(0);
    }
    if (args[1].type != VAL_NUM) return bval_num(0);

    ListData* lst = args[0].list;
    int idx = (int)args[1].num;
    if (idx < 0 || idx >= lst->len) return bval_num(0);

    return bval_num(lst->data[idx]);
}

// ====================================================================
// 14. list_set(list, index, val) — write list element
// ====================================================================

static Value bif_list_set(Value* args, int n, ExecutionContext* ctx)
{
    (void)ctx;
    if (n < 3 || args[0].type != VAL_LIST || !args[0].list) {
        return bval_undefined();
    }
    if (args[1].type != VAL_NUM) return bval_undefined();

    ListData* lst = args[0].list;
    int idx = (int)args[1].num;
    if (idx < 0 || idx >= lst->len) return bval_undefined();

    double val = (args[2].type == VAL_NUM) ? args[2].num : 0.0;
    lst->data[idx] = val;

    return bval_list(lst);
}

// ====================================================================
// 15. list_len(list) — number of elements
// ====================================================================

static Value bif_list_len(Value* args, int n, ExecutionContext* ctx)
{
    (void)ctx;
    if (n < 1 || args[0].type != VAL_LIST || !args[0].list) {
        return bval_num(0);
    }
    return bval_num((double)args[0].list->len);
}

// ====================================================================
// 16. remote_read_avg(ids...) — average of multiple sensor readings
// ====================================================================

static Value bif_remote_read_avg(Value* args, int n, ExecutionContext* ctx)
{
    uint8_t ids[MAX_PARSE_IDS];
    int id_count = parse_module_ids(args, n, ids, MAX_PARSE_IDS);
    if (id_count == 0) return bval_num(0);

    double sum   = 0.0;
    int    valid = 0;
    bool   limit_hit = false;
    double values[DATA_RESP_MAX_VALUES];
    for (int i = 0; i < id_count; i++) {
        if (ctx->constraint_violated) break;
        if (sensor_call_check(ctx)) { limit_hit = true; break; }
        int cnt = espnow_comm_request_read(ids[i], values, DATA_RESP_MAX_VALUES);
        if (cnt > 0) { sum += values[0]; valid++; }
    }

    // Aggregation completed (possibly partial).  Don't let the sensor
    // call limit abort the entire script — the partial average is valid.
    if (limit_hit) ctx->constraint_violated = false;

    return bval_num((valid > 0) ? (sum / (double)valid) : 0.0);
}

// ====================================================================
// 17. remote_read_max(ids...) — maximum of multiple sensor readings
// ====================================================================

static Value bif_remote_read_max(Value* args, int n, ExecutionContext* ctx)
{
    uint8_t ids[MAX_PARSE_IDS];
    int id_count = parse_module_ids(args, n, ids, MAX_PARSE_IDS);
    if (id_count == 0) return bval_num(0);

    double max_val = -1e308;
    int    valid   = 0;
    bool   limit_hit = false;
    double values[DATA_RESP_MAX_VALUES];
    for (int i = 0; i < id_count; i++) {
        if (ctx->constraint_violated) break;
        if (sensor_call_check(ctx)) { limit_hit = true; break; }
        int cnt = espnow_comm_request_read(ids[i], values, DATA_RESP_MAX_VALUES);
        if (cnt > 0) { max_val = (valid == 0 || values[0] > max_val) ? values[0] : max_val; valid++; }
    }

    if (limit_hit) ctx->constraint_violated = false;
    return bval_num((valid > 0) ? max_val : 0.0);
}

// ====================================================================
// 18. remote_read_min(ids...) — minimum of multiple sensor readings
// ====================================================================

static Value bif_remote_read_min(Value* args, int n, ExecutionContext* ctx)
{
    uint8_t ids[MAX_PARSE_IDS];
    int id_count = parse_module_ids(args, n, ids, MAX_PARSE_IDS);
    if (id_count == 0) return bval_num(0);

    double min_val = 1e308;
    int    valid   = 0;
    bool   limit_hit = false;
    double values[DATA_RESP_MAX_VALUES];
    for (int i = 0; i < id_count; i++) {
        if (ctx->constraint_violated) break;
        if (sensor_call_check(ctx)) { limit_hit = true; break; }
        int cnt = espnow_comm_request_read(ids[i], values, DATA_RESP_MAX_VALUES);
        if (cnt > 0) { min_val = (valid == 0 || values[0] < min_val) ? values[0] : min_val; valid++; }
    }

    if (limit_hit) ctx->constraint_violated = false;
    return bval_num((valid > 0) ? min_val : 0.0);
}

// ====================================================================
// 19. list_free(list) — release list pool slot
// ====================================================================

static Value bif_list_free_builtin(Value* args, int n, ExecutionContext* ctx)
{
    (void)ctx;
    if (!check_args("list_free", n, 1)) return bval_undefined();
    if (args[0].type == VAL_LIST) {
        bif_pool_free(args[0].list);
    }
    return bval_undefined();
}

// ====================================================================
// 20. read_sensor(id_or_name) — read cached remote sensor value (synced with LCD)
// ====================================================================

static Value bif_read_sensor(Value* args, int n, ExecutionContext* ctx)
{
    (void)ctx;
    if (n < 1) return bval_num(0);

    uint8_t module_id = 0;
    if (!resolve_module_id_arg("read_sensor", &args[0], ctx, &module_id)) {
        return bval_num(0);
    }

    if (g_sensor_read_callback != NULL) {
        double values[16];
        int count = g_sensor_read_callback(module_id, values, 16);
        if (count > 0) {
            return bval_num(values[0]);  // always single value
        }
    }

    return bval_num(0);
}

// ====================================================================
// 21. send_motor(pin, speed) — alias for analog_write (PWM)
// ====================================================================

static Value bif_send_motor(Value* args, int n, ExecutionContext* ctx)
{
    // Delegates to analog_write
    return bif_analog_write(args, n, ctx);
}

// ====================================================================
// 22. mic_level() - INMP441 I2S microphone level, 0..100 percent FS
// ====================================================================

static Value bif_mic_level(Value* args, int n, ExecutionContext* ctx)
{
    (void)args;
    (void)n;
    (void)ctx;
    return bval_num(hw_mic_level());
}

// ====================================================================
// 23. buzzer_note(id, note_id, duration_ms) - remote passive buzzer note
// ====================================================================

static Value bif_buzzer_note(Value* args, int n, ExecutionContext* ctx)
{
    if (!check_args("buzzer_note", n, 3) ||
        args[1].type != VAL_NUM || args[2].type != VAL_NUM) {
        return bval_num(-1);
    }

    uint8_t module_id = 0;
    if (!resolve_module_id_arg("buzzer_note", &args[0], ctx, &module_id)) {
        return bval_num(-1);
    }

    int note_id = (int)args[1].num;
    if (note_id < 0) note_id = 0;
    if (note_id > 36) note_id = 36;

    int duration_ms = (int)args[2].num;
    if (duration_ms < 1) duration_ms = 1;
    if (duration_ms > 5000) duration_ms = 5000;

    uint8_t payload[3] = {
        (uint8_t)note_id,
        (uint8_t)((duration_ms >> 8) & 0xff),
        (uint8_t)(duration_ms & 0xff),
    };
    esp_err_t err = espnow_comm_send_cmd(module_id, CMD_BUZZER_NOTE,
                                         payload, sizeof(payload));
    return bval_num((double)err);
}

// ====================================================================
// 24. buzzer_beep(id, count [, note_id, duration_ms]) - repeated beeps
// ====================================================================

static Value bif_buzzer_beep(Value* args, int n, ExecutionContext* ctx)
{
    if (!check_args("buzzer_beep", n, 2) || args[1].type != VAL_NUM) {
        return bval_num(-1);
    }

    uint8_t module_id = 0;
    if (!resolve_module_id_arg("buzzer_beep", &args[0], ctx, &module_id)) {
        return bval_num(-1);
    }

    int count = (int)args[1].num;
    if (count < 1) count = 1;
    if (count > 10) count = 10;

    int note_id = BUZZER_NOTE_G5;
    if (n >= 3 && args[2].type == VAL_NUM) {
        note_id = (int)args[2].num;
    }
    if (note_id < 0) note_id = 0;
    if (note_id > 36) note_id = 36;

    int duration_ms = BUZZER_DEFAULT_MS;
    if (n >= 4 && args[3].type == VAL_NUM) {
        duration_ms = (int)args[3].num;
    }
    if (duration_ms < 1) duration_ms = 1;
    if (duration_ms > 5000) duration_ms = 5000;

    esp_err_t last_err = ESP_OK;
    uint8_t payload[3] = {
        (uint8_t)note_id,
        (uint8_t)((duration_ms >> 8) & 0xff),
        (uint8_t)(duration_ms & 0xff),
    };

    for (int i = 0; i < count; i++) {
        last_err = espnow_comm_send_cmd(module_id, CMD_BUZZER_NOTE,
                                        payload, sizeof(payload));
        if (i + 1 < count) {
            vTaskDelay(pdMS_TO_TICKS(duration_ms + 120));
        }
    }

    return bval_num((double)last_err);
}

// ====================================================================
// 25. buzzer_song(id, song_index) - remote passive buzzer preset song
// ====================================================================

static Value bif_buzzer_song(Value* args, int n, ExecutionContext* ctx)
{
    if (!check_args("buzzer_song", n, 2) || args[1].type != VAL_NUM) {
        return bval_num(-1);
    }

    uint8_t module_id = 0;
    if (!resolve_module_id_arg("buzzer_song", &args[0], ctx, &module_id)) {
        return bval_num(-1);
    }

    int song_index = (int)args[1].num;
    if (song_index < 0) song_index = 0;
    if (song_index > 2) song_index = 2;

    uint8_t payload[1] = { (uint8_t)song_index };
    esp_err_t err = espnow_comm_send_cmd(module_id, CMD_BUZZER_SONG,
                                         payload, sizeof(payload));
    return bval_num((double)err);
}

// ====================================================================
// 26. servo_write(id, angle) - remote 0-180 degree servo position
// ====================================================================

static Value bif_servo_write(Value* args, int n, ExecutionContext* ctx)
{
    if (!check_args("servo_write", n, 2) || args[1].type != VAL_NUM) {
        return bval_num(-1);
    }

    uint8_t module_id = 0;
    if (!resolve_module_id_arg("servo_write", &args[0], ctx, &module_id)) {
        return bval_num(-1);
    }

    int angle = (int)args[1].num;
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;

    uint8_t payload[1] = { (uint8_t)angle };
    esp_err_t err = espnow_comm_send_cmd(module_id, CMD_SERVO_WRITE,
                                         payload, sizeof(payload));
    return bval_num((double)err);
}

// ====================================================================
// 27. servo_sweep(id, from, to, step, delay_ms) - repeated servo_write
// ====================================================================

static Value bif_servo_sweep(Value* args, int n, ExecutionContext* ctx)
{
    if (!check_args("servo_sweep", n, 5) ||
        args[1].type != VAL_NUM || args[2].type != VAL_NUM ||
        args[3].type != VAL_NUM || args[4].type != VAL_NUM) {
        return bval_num(-1);
    }

    uint8_t module_id = 0;
    if (!resolve_module_id_arg("servo_sweep", &args[0], ctx, &module_id)) {
        return bval_num(-1);
    }

    int from = (int)args[1].num;
    int to = (int)args[2].num;
    int step = (int)args[3].num;
    int delay_ms = (int)args[4].num;

    if (from < 0) from = 0;
    if (from > 180) from = 180;
    if (to < 0) to = 0;
    if (to > 180) to = 180;
    if (step == 0) step = (to >= from) ? 10 : -10;
    if (to > from && step < 0) step = -step;
    if (to < from && step > 0) step = -step;
    if (delay_ms < 20) delay_ms = 20;
    if (delay_ms > 5000) delay_ms = 5000;

    esp_err_t last_err = ESP_OK;
    for (int angle = from;; angle += step) {
        uint8_t payload[1] = { (uint8_t)angle };
        last_err = espnow_comm_send_cmd(module_id, CMD_SERVO_WRITE,
                                        payload, sizeof(payload));
        if ((step > 0 && angle >= to) || (step < 0 && angle <= to)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        int next = angle + step;
        if ((step > 0 && next > to) || (step < 0 && next < to)) {
            angle = to - step;
        }
    }

    return bval_num((double)last_err);
}

// ====================================================================
// 28. pump_write(id, duration_ms) — remote water pump timed control
//     duration_ms = 0           → turn off immediately
//     duration_ms = 1..30000    → turn on for N ms, sensor auto-off
// ====================================================================

static Value bif_pump_write(Value* args, int n, ExecutionContext* ctx)
{
    if (!check_args("pump_write", n, 2) || args[1].type != VAL_NUM) {
        return bval_num(-1);
    }

    uint8_t module_id = 0;
    if (!resolve_module_id_arg("pump_write", &args[0], ctx, &module_id)) {
        return bval_num(-1);
    }

    if (!isfinite(args[1].num) || args[1].num < 0 ||
        args[1].num > 30000) {
        ESP_LOGW(TAG, "pump_write duration must be 0..30000 ms");
        return bval_num(-1);
    }
    int dur = (int)args[1].num;

    uint16_t duration_ms = (uint16_t)dur;
    uint8_t payload[2] = { (uint8_t)(duration_ms >> 8),
                           (uint8_t)(duration_ms & 0xFF) };
    esp_err_t err = espnow_comm_send_cmd(module_id, CMD_PUMP_WRITE,
                                         payload, sizeof(payload));
    return bval_num((double)err);
}
