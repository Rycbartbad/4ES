#pragma once

#include "sdkconfig.h"
#include <stdint.h>
#include "interpreter/ast.h"
#include "interpreter/environment.h"

#ifdef __cplusplus
extern "C" {
#endif

// Runtime constraints — design.md §6.7
#define MAX_NESTED_LOOPS 16

typedef struct {
    int exec_depth;
    int total_statements;
    int loop_iterations[MAX_NESTED_LOOPS];
    int loop_depth;
    int sensor_calls_total;
    bool constraint_violated;
    const char* violation_msg;
    bool has_returned;
    Value return_value;

    // Watchdog integration (global atomic flag)
    volatile bool* s_script_timeout_ptr;

    // Environment pool tracking
    int env_pool_used;

    // Loop tracking for optimizations (G7)
    bool inside_loop;

    // Last call name for consecutive remote_read detection (G6)
    const char* last_call_name;
} ExecutionContext;

void ctx_init(ExecutionContext* ctx);
void ctx_reset(ExecutionContext* ctx);

// Main execution entry
Value execute(ASTNode* ast, Environment* env, ExecutionContext* ctx);

// Script lifecycle guards — design.md §15.3
typedef struct {
    int ast_node_count;
    int ast_tree_depth;
    int global_bindings_used;   // current env binding count
    int funcs_defined;          // FUNC_DEF nodes in AST
    int list_elements_total;    // sum of list_new(size) params
    bool has_list_in_loop;      // list_new inside while body
    int max_parse_depth;        // max depth from AST walk
    bool passed;                // overall validation result
    const char* fail_reason;    // human-readable failure reason
} ResourceReport;

ResourceReport validate_resources(ASTNode* ast, Environment* env);

#ifdef __cplusplus
}
#endif

// Pin tracking for hardware safety on script end.
// Called by builtins (digital_write, analog_write, send_motor) to register
// output pins that should be reset when the script ends (normally or via abort).
// Non-C-linkage — defined in app_main.cpp.
void track_output_pin(uint8_t pin);
