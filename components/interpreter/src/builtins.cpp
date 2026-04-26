/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO V1.0 — Built-in functions (P6)
 *
 * 21 builtins registered into the global environment:
 *
 *   GPIO/ADC/PWM:      digital_read, digital_write, analog_read, analog_write
 *   Timing:            sleep
 *   I/O:               print
 *   ESP-NOW peer mgr:  list_peers, peer_count, peer_online
 *   ESP-NOW comm:      remote_read, espnow_send
 *   List ops:          list_new, list_get, list_set, list_len, list_free
 *   Aggregation:       remote_read_avg, remote_read_max, remote_read_min
 *   Sensor aliases:    read_sensor, send_motor
 */

#include "sdkconfig.h"

#include "interpreter/builtins.h"
#include "interpreter/interpreter.h"
#include "interpreter/value.h"
#include "interpreter/intern.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hw_drivers/drivers.h"

#include "espnow_comm/comm.h"
#include "espnow_comm/peer_mgr.h"

#include "esp_log.h"

static const char* TAG = "builtins";

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

typedef Value (*BuiltinFunc)(Value* args, int arg_count, ExecutionContext* ctx);

typedef struct {
    const char* name;
    int         param_count;
    BuiltinFunc func;
} BuiltinEntry;

// ---- Forward declarations of all 21 builtin implementations ---------

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
    {"read_sensor",      1, bif_read_sensor},
    {"send_motor",       2, bif_send_motor},
};

// ---- Persistent FuncObj array for builtins (body = NULL = builtin) ---

static FuncObj s_builtin_funcs[BIF_COUNT];
static bool    s_builtins_registered = false;

// ====================================================================
// register_builtins — called once at startup
// ====================================================================

void register_builtins(Environment* env)
{
    if (s_builtins_registered) return;
    s_builtins_registered = true;

    for (int i = 0; i < BIF_COUNT; i++) {
        FuncObj* fo = &s_builtin_funcs[i];
        fo->name        = intern_string(s_builtin_entries[i].name,
                                        (int)strlen(s_builtin_entries[i].name));
        fo->param_count = s_builtin_entries[i].param_count;
        fo->body        = NULL;             // signals builtin to interpreter

        Value v;
        v.type = VAL_FUNC;
        v.func = fo;
        env_define(env, s_builtin_entries[i].name, v);
    }
}

// ====================================================================
// Optional print capture callback — set by web_console (P7.5)
// Defined here, non-NULL when web_console is active.
// ====================================================================
void (*g_print_callback)(const char* str, int len) = NULL;

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

    // Dispatch by name using strcmp chain
    if (strcmp(name, "digital_read")    == 0) return bif_digital_read(local_args, n, ctx);
    if (strcmp(name, "digital_write")   == 0) return bif_digital_write(local_args, n, ctx);
    if (strcmp(name, "analog_read")     == 0) return bif_analog_read(local_args, n, ctx);
    if (strcmp(name, "analog_write")    == 0) return bif_analog_write(local_args, n, ctx);
    if (strcmp(name, "sleep")           == 0) return bif_sleep(local_args, n, ctx);
    if (strcmp(name, "print")           == 0) return bif_print(local_args, n, ctx);
    if (strcmp(name, "list_peers")      == 0) return bif_list_peers(local_args, n, ctx);
    if (strcmp(name, "peer_count")      == 0) return bif_peer_count(local_args, n, ctx);
    if (strcmp(name, "peer_online")     == 0) return bif_peer_online(local_args, n, ctx);
    if (strcmp(name, "remote_read")     == 0) return bif_remote_read(local_args, n, ctx);
    if (strcmp(name, "espnow_send")     == 0) return bif_espnow_send(local_args, n, ctx);
    if (strcmp(name, "list_new")        == 0) return bif_list_new(local_args, n, ctx);
    if (strcmp(name, "list_get")        == 0) return bif_list_get(local_args, n, ctx);
    if (strcmp(name, "list_set")        == 0) return bif_list_set(local_args, n, ctx);
    if (strcmp(name, "list_len")        == 0) return bif_list_len(local_args, n, ctx);
    if (strcmp(name, "remote_read_avg") == 0) return bif_remote_read_avg(local_args, n, ctx);
    if (strcmp(name, "remote_read_max") == 0) return bif_remote_read_max(local_args, n, ctx);
    if (strcmp(name, "remote_read_min") == 0) return bif_remote_read_min(local_args, n, ctx);
    if (strcmp(name, "list_free")       == 0) return bif_list_free_builtin(local_args, n, ctx);
    if (strcmp(name, "read_sensor")     == 0) return bif_read_sensor(local_args, n, ctx);
    if (strcmp(name, "send_motor")      == 0) return bif_send_motor(local_args, n, ctx);

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
        char* token = strtok(buf, ",");
        while (token && id_count < ids_max) {
            // Trim leading whitespace
            while (*token == ' ' || *token == '\t') token++;
            if (*token >= '0' && *token <= '9') {
                ids_out[id_count++] = (uint8_t)atoi(token);
            }
            token = strtok(NULL, ",");
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
    (void)ctx;
    if (n >= 1 && args[0].type == VAL_NUM) {
        int ms = (int)args[0].num;
        if (ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(ms));
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
                v->num < 1e9 && v->num > -1e9) {
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
// 10. remote_read(module_id [, pin]) — synchronous ESP-NOW sensor read
// ====================================================================

static Value bif_remote_read(Value* args, int n, ExecutionContext* ctx)
{
    if (n < 1) return bval_num(0);

    // Check sensor call limit
    if (sensor_call_check(ctx)) return bval_num(0);

    uint8_t module_id = 0;
    uint8_t pin       = 0;
    bool    found     = false;

    if (args[0].type == VAL_NUM) {
        module_id = (uint8_t)args[0].num;
        found     = true;
        if (n >= 2 && args[1].type == VAL_NUM) {
            pin = (uint8_t)args[1].num;
        }
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
            if (n >= 2 && args[1].type == VAL_NUM) {
                pin = (uint8_t)args[1].num;
            }
        }
    }

    if (found) {
        return bval_num(espnow_comm_request_read(module_id, pin));
    }
    return bval_num(0);
}

// ====================================================================
// 11. espnow_send(module_id, cmd_id [, payload...]) — send arbitrary command
// ====================================================================

static Value bif_espnow_send(Value* args, int n, ExecutionContext* ctx)
{
    (void)ctx;
    if (n < 2 || args[0].type != VAL_NUM || args[1].type != VAL_NUM) {
        return bval_num(-1);
    }

    uint8_t  module_id = (uint8_t)args[0].num;
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
    for (int i = 0; i < id_count; i++) {
        if (ctx->constraint_violated) break;
        if (sensor_call_check(ctx)) break;
        double val = espnow_comm_request_read(ids[i], 0);
        sum += val;
        valid++;
    }

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
    for (int i = 0; i < id_count; i++) {
        if (ctx->constraint_violated) break;
        if (sensor_call_check(ctx)) break;
        double val = espnow_comm_request_read(ids[i], 0);
        if (valid == 0 || val > max_val) max_val = val;
        valid++;
    }

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
    for (int i = 0; i < id_count; i++) {
        if (ctx->constraint_violated) break;
        if (sensor_call_check(ctx)) break;
        double val = espnow_comm_request_read(ids[i], 0);
        if (valid == 0 || val < min_val) min_val = val;
        valid++;
    }

    return bval_num((valid > 0) ? min_val : 0.0);
}

// ====================================================================
// 19. list_free(list) — release list pool slot
// ====================================================================

static Value bif_list_free_builtin(Value* args, int n, ExecutionContext* ctx)
{
    (void)ctx;
    if (n >= 1 && args[0].type == VAL_LIST) {
        bif_pool_free(args[0].list);
    }
    return bval_undefined();
}

// ====================================================================
// 20. read_sensor(pin) — alias for analog_read
// ====================================================================

static Value bif_read_sensor(Value* args, int n, ExecutionContext* ctx)
{
    // Delegates to analog_read
    return bif_analog_read(args, n, ctx);
}

// ====================================================================
// 21. send_motor(pin, speed) — alias for analog_write (PWM)
// ====================================================================

static Value bif_send_motor(Value* args, int n, ExecutionContext* ctx)
{
    // Delegates to analog_write
    return bif_analog_write(args, n, ctx);
}
