#pragma once

#include "sdkconfig.h"
#include "interpreter/environment.h"
#include "interpreter/interpreter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BIF_COUNT 28

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
