#pragma once

#include "sdkconfig.h"
#include "interpreter/environment.h"
#include "interpreter/interpreter.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BIF_COUNT 28
#define CONTROL_COMMAND_COUNT 7
#define CONTROL_COMMAND_MAX_ARGS 5

typedef enum {
    CONTROL_ARG_DEVICE = 0,
    CONTROL_ARG_NUMBER,
} ControlArgKind;

typedef struct {
    const char* name;
    ControlArgKind kind;
    double min_value;
    double max_value;
    const char* description;
} ControlArgSpec;

typedef Value (*ControlCommandHandler)(Value* args, int arg_count,
                                       ExecutionContext* ctx);

typedef struct {
    const char* dsl_name;
    const char* device_type;
    const char* description;
    const ControlArgSpec* args;
    uint8_t arg_count;
    bool print_result;
    ControlCommandHandler handler;
} ControlCommandSpec;

// Read-only catalog used by interpreter dispatch and Web Console AI tooling.
const ControlCommandSpec* control_command_specs(size_t* out_count);
const ControlCommandSpec* control_command_find(const char* dsl_name);

// Format already-validated argument literals into one executable DSL
// statement. Returns the number of bytes written, or -1 on invalid input.
int control_command_format_dsl(const char* dsl_name,
                               const char* const* arg_literals,
                               size_t arg_count,
                               char* out, size_t out_len);

// Register all built-in functions into the given environment.
void register_builtins(Environment* env);

// Builtin dispatch — called from interpreter.cpp call_builtin().
// Receives already-evaluated arguments.
Value call_builtin_by_name(const char* name, const Value* args,
                           int arg_count, ExecutionContext* ctx);

// List pool management — pool lives in interpreter.cpp, exposed for builtins.
ListData* bif_pool_alloc(void);
void      bif_pool_free(ListData* lst);

// Reset builtin-internal state between script executions.
void bif_pools_reset(void);

// Optional callback for print() output capture (used by web_console).
// Set to NULL (default) to skip; set to a function pointer to capture.
// The callback receives the formatted output string and its length.
extern void (*g_print_callback)(const char* str, int len);

#ifdef __cplusplus
}
#endif
