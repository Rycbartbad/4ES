#include "sdkconfig.h"
#include "interpreter/interpreter.h"
#include "interpreter/builtins.h"
#include "interpreter/intern.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stddef.h>

#include "esp_log.h"

static const char* TAG = "interpreter";

// ====================================================================
// Compile-time defaults (when sdkconfig.h not yet configured)
// ====================================================================

#ifndef CONFIG_LIST_POOL_SIZE
#define CONFIG_LIST_POOL_SIZE 8
#endif
#ifndef CONFIG_FUNC_POOL_SIZE
#define CONFIG_FUNC_POOL_SIZE 16
#endif
#ifndef CONFIG_MAX_FUNC_PARAMS
#define CONFIG_MAX_FUNC_PARAMS 8
#endif
#ifndef CONFIG_MAX_EXEC_DEPTH
#define CONFIG_MAX_EXEC_DEPTH 64
#endif
#ifndef CONFIG_MAX_LOOP_ITERATIONS
#define CONFIG_MAX_LOOP_ITERATIONS 10000
#endif
#ifndef CONFIG_MAX_EXEC_STATEMENTS
#define CONFIG_MAX_EXEC_STATEMENTS 50000
#endif
#ifndef CONFIG_MAX_SENSOR_CALLS_PER_SCRIPT
#define CONFIG_MAX_SENSOR_CALLS_PER_SCRIPT 20
#endif

// ====================================================================
// Forward declarations
// ====================================================================

static int  compute_tree_depth(const ASTNode* node);

// WalkState for validate_resources AST traversal
typedef struct {
    int  funcs_defined;
    int  list_elements_total;
    bool has_list_in_loop;
    bool inside_while;
    int  max_depth;
} WalkState;

static void walk_validate(const ASTNode* node, WalkState* ws, int depth);

// ====================================================================
// Value construction helpers (avoid C++20 mixed-designator issue)
// ====================================================================

static inline Value val_undefined(void)
{
    Value v;
    v.type = VAL_UNDEFINED;
    v.num  = 0;
    return v;
}

static inline Value val_num(double n)
{
    Value v;
    v.type = VAL_NUM;
    v.num  = n;
    return v;
}

static inline Value val_bool(bool b)
{
    Value v;
    v.type = VAL_BOOL;
    v.b    = b;
    return v;
}

static inline Value val_str(const char* s)
{
    Value v;
    v.type = VAL_STR;
    v.str  = s;
    return v;
}

// Format scalar values for string concatenation. Keep number formatting
// consistent with print(), so sensor values do not unexpectedly gain a
// trailing ".000000" when embedded in a label.
static int format_scalar_text(Value value, char* out, size_t out_size)
{
    switch (value.type) {
    case VAL_STR:
        return snprintf(out, out_size, "%s", value.str ? value.str : "");
    case VAL_NUM: {
        double intpart;
        if (modf(value.num, &intpart) == 0.0 &&
            value.num < 1e10 && value.num > -1e10) {
            return snprintf(out, out_size, "%.0f", value.num);
        }
        return snprintf(out, out_size, "%g", value.num);
    }
    case VAL_BOOL:
        return snprintf(out, out_size, "%s", value.b ? "true" : "false");
    default:
        return -1;
    }
}

// ====================================================================
// Static object pools --- design.md section 6.6
// ====================================================================

static ListData s_list_pool[CONFIG_LIST_POOL_SIZE];
static bool     s_list_used[CONFIG_LIST_POOL_SIZE];

static FuncObj  s_func_pool[CONFIG_FUNC_POOL_SIZE];
static bool     s_func_used[CONFIG_FUNC_POOL_SIZE];

// ====================================================================
// Pool allocation helpers
// ====================================================================

// --- List pool API exposed for builtins (bif_pool_alloc / bif_pool_free) ---

ListData* bif_pool_alloc(void)
{
    for (int i = 0; i < CONFIG_LIST_POOL_SIZE; i++) {
        if (!s_list_used[i]) {
            s_list_used[i] = true;
            memset(&s_list_pool[i], 0, sizeof(ListData));
            return &s_list_pool[i];
        }
    }
    return NULL;
}

void bif_pool_free(ListData* lst)
{
    if (!lst) return;
    for (int i = 0; i < CONFIG_LIST_POOL_SIZE; i++) {
        if (&s_list_pool[i] == lst) {
            s_list_used[i] = false;
            // Zero the slot to make any dangling reference safely inert.
            // len=0 ensures list_get/set bounds-check fail gracefully.
            // This is a best-effort guard, not a full use-after-free
            // prevention (a subsequent list_new may reuse this slot).
            memset(&s_list_pool[i], 0, sizeof(ListData));
            return;
        }
    }
}

static FuncObj* func_alloc(void)
{
    for (int i = 0; i < CONFIG_FUNC_POOL_SIZE; i++) {
        if (!s_func_used[i]) {
            s_func_used[i] = true;
            memset(&s_func_pool[i], 0, sizeof(FuncObj));
            return &s_func_pool[i];
        }
    }
    return NULL;
}

// --- Pool reset — called between scripts to free all pool slots --------

void bif_pools_reset(void)
{
    memset(s_list_used, 0, sizeof(s_list_used));
    memset(s_func_used, 0, sizeof(s_func_used));
}

// ====================================================================
// Forward declarations
// ====================================================================

static Value eval_expr(ASTNode* node, Environment* env, ExecutionContext* ctx);
static void execute_statement(ASTNode* node, Environment* env, ExecutionContext* ctx);
static void execute_block_stmts(ASTNode** stmts, int stmt_count,
                                Environment* env, ExecutionContext* ctx);
static Value call_function(const char* name, ASTNode** args, int arg_count,
                           Environment* env, ExecutionContext* ctx);
static Value call_builtin(const char* name, const Value* args, int arg_count,
                          ExecutionContext* ctx);

// ====================================================================
// Value helpers
// ====================================================================

static bool is_truthy(Value v)
{
    switch (v.type) {
        case VAL_NUM:   return v.num != 0.0;
        case VAL_BOOL:  return v.b;
        case VAL_STR:   return v.str != NULL && v.str[0] != '\0';
        case VAL_LIST:  return v.list != NULL && v.list->len > 0;
        case VAL_FUNC:  return v.func != NULL;
        default:        return false;
    }
}

static bool values_equal(Value a, Value b)
{
    if (a.type != b.type) return false;
    switch (a.type) {
        case VAL_NUM:
            return a.num == b.num;
        case VAL_BOOL:
            return a.b == b.b;
        case VAL_STR:
            if (a.str == NULL && b.str == NULL) return true;
            if (a.str == NULL || b.str == NULL) return false;
            return strcmp(a.str, b.str) == 0;
        case VAL_LIST:
            return a.list == b.list;
        case VAL_FUNC:
            return a.func == b.func;
        default:
            return false;
    }
}

// ====================================================================
// ctx_init / ctx_reset
// ====================================================================

void ctx_init(ExecutionContext* ctx)
{
    memset(ctx, 0, sizeof(ExecutionContext));
}

void ctx_reset(ExecutionContext* ctx)
{
    ctx->exec_depth          = 0;
    ctx->total_statements    = 0;
    ctx->loop_depth          = 0;
    ctx->sensor_calls_total  = 0;
    ctx->constraint_violated = false;
    ctx->violation_msg       = NULL;
    ctx->has_returned        = false;
    ctx->env_pool_used       = 0;
    ctx->last_call_name      = NULL;
    ctx->inside_loop         = false;

    memset(ctx->loop_iterations, 0, sizeof(ctx->loop_iterations));

    // Reset env pool (function nesting) so sequential scripts don't exhaust slots.
    env_pool_reset();

    // Reset builtin pools (list + func slots)
    bif_pools_reset();
}

// ====================================================================
// Expression evaluation
// ====================================================================

static Value eval_expr(ASTNode* node, Environment* env, ExecutionContext* ctx)
{
    if (!node || ctx->constraint_violated) {
        return val_undefined();
    }

    switch (node->type) {

    case NODE_LITERAL_NUM:
        return val_num(node->num_val);

    case NODE_LITERAL_STR:
        return val_str(node->str_val);

    case NODE_LITERAL_BOOL:
        return val_bool(node->bool_val);

    case NODE_IDENT: {
        Value v = env_get(env, node->str_val);
        if (v.type == VAL_UNDEFINED) {
#if CONFIG_STRICT_MODE
            ctx->constraint_violated = true;
            ctx->violation_msg       = "Undefined variable";
#endif
        }
        return v;
    }

    case NODE_BINARY_OP: {
        // --- Short-circuit logical operators ---
        if (node->op == TOKEN_AND) {
            Value left = eval_expr(node->left, env, ctx);
            if (ctx->constraint_violated) return val_undefined();
            if (!is_truthy(left)) return left;
            return eval_expr(node->right, env, ctx);
        }

        if (node->op == TOKEN_OR) {
            Value left = eval_expr(node->left, env, ctx);
            if (ctx->constraint_violated) return val_undefined();
            if (is_truthy(left)) return left;
            return eval_expr(node->right, env, ctx);
        }

        // --- Non-short-circuit: evaluate both sides ---
        Value left  = eval_expr(node->left,  env, ctx);
        if (ctx->constraint_violated) return val_undefined();
        Value right = eval_expr(node->right, env, ctx);
        if (ctx->constraint_violated) return val_undefined();

        switch (node->op) {

        // String concatenation. If either side is a string, scalar numbers
        // and booleans are converted to text. This supports natural scripts
        // such as print("Temperature: " + read_sensor(1)).
        case TOKEN_PLUS: {
            if (left.type == VAL_STR || right.type == VAL_STR) {
                char left_text[INTERN_ENTRY_LEN];
                char right_text[INTERN_ENTRY_LEN];
                int left_len = format_scalar_text(left, left_text,
                                                   sizeof(left_text));
                int right_len = format_scalar_text(right, right_text,
                                                    sizeof(right_text));
                if (left_len < 0 || right_len < 0) {
#if CONFIG_STRICT_MODE
                    ctx->constraint_violated = true;
                    ctx->violation_msg       = "Type error: cannot concatenate this value";
#endif
                    return val_undefined();
                }

                char buf[INTERN_ENTRY_LEN];
                int n = snprintf(buf, sizeof(buf), "%s%s",
                                 left_text, right_text);
                if (n < 0 || n >= (int)sizeof(buf)) {
#if CONFIG_STRICT_MODE
                    ctx->constraint_violated = true;
                    ctx->violation_msg       = "Type error: concatenation result too long";
#endif
                    return val_undefined();
                }
                return val_str(intern_string(buf, n));
            }
            if (left.type == VAL_NUM && right.type == VAL_NUM) {
                return val_num(left.num + right.num);
            }
#if CONFIG_STRICT_MODE
            ctx->constraint_violated = true;
            ctx->violation_msg       = "Type error: arithmetic needs numbers";
#endif
            return val_num(0.0);
        }

        // Arithmetic (minus, multiply, divide — numeric only)
        case TOKEN_MINUS:
        case TOKEN_STAR:
        case TOKEN_SLASH: {
            if (left.type != VAL_NUM || right.type != VAL_NUM) {
#if CONFIG_STRICT_MODE
                ctx->constraint_violated = true;
                ctx->violation_msg       = "Type error: arithmetic needs numbers";
#endif
                return val_num(0.0);
            }
            double a = left.num;
            double b = right.num;
            switch (node->op) {
                case TOKEN_MINUS: return val_num(a - b);
                case TOKEN_STAR:  return val_num(a * b);
                case TOKEN_SLASH: return val_num((b != 0.0) ? a / b : 0.0);
                default:          return val_num(0.0);
            }
        }

        // Equality
        case TOKEN_EQ:
            return val_bool(values_equal(left, right));
        case TOKEN_NEQ:
            return val_bool(!values_equal(left, right));

        // Relational (cross-type: convert to double)
        case TOKEN_LT:
        case TOKEN_GT:
        case TOKEN_LE:
        case TOKEN_GE: {
            double a = (left.type  == VAL_NUM) ? left.num  : 0.0;
            double b = (right.type == VAL_NUM) ? right.num : 0.0;
            switch (node->op) {
                case TOKEN_LT: return val_bool(a <  b);
                case TOKEN_GT: return val_bool(a >  b);
                case TOKEN_LE: return val_bool(a <= b);
                case TOKEN_GE: return val_bool(a >= b);
                default:       return val_bool(false);
            }
        }

        default:
            return val_undefined();
        }
    }

    case NODE_UNARY_OP: {
        Value operand = eval_expr(node->left, env, ctx);
        if (ctx->constraint_violated) return val_undefined();

        if (node->op == TOKEN_NOT) {
            return val_bool(!is_truthy(operand));
        }

        if (node->op == TOKEN_MINUS) {
            double val = (operand.type == VAL_NUM) ? operand.num : 0.0;
            return val_num(-val);
        }

        return val_undefined();
    }

    case NODE_FUNC_CALL: {
        return call_function(node->name, node->args,
                            node->arg_count, env, ctx);
    }

    case NODE_ASSIGN: {
        // Assignment as expression (e.g. x = y = 5)
        if (!node->left || node->left->type != NODE_IDENT) {
            return val_undefined();
        }
        const char* name = node->left->str_val;
        Value val = eval_expr(node->right, env, ctx);
        if (ctx->constraint_violated) return val_undefined();
        int ret = env_set(env, name, val);
        if (ret < 0) {
#if CONFIG_STRICT_MODE
            ctx->constraint_violated = true;
            ctx->violation_msg       = "Undefined variable";
#endif
        }
        return val;
    }

    default:
        return val_undefined();
    }
}

// ====================================================================
// Statement execution
// ====================================================================

static void execute_statement(ASTNode* node, Environment* env,
                              ExecutionContext* ctx)
{
    if (!node || ctx->constraint_violated) return;

    switch (node->type) {

    case NODE_VAR_DECL: {
        // Parser stores var name in func_name, init expr in func_body
        Value init_val = val_undefined();

        if (node->func_body) {
            init_val = eval_expr(node->func_body, env, ctx);
            if (ctx->constraint_violated) return;
        }

        int ret = env_define(env, node->func_name, init_val);
        if (ret < 0) {
            ctx->constraint_violated = true;
            ctx->violation_msg       = "Environment full";
        }
        return;
    }

    case NODE_ASSIGN: {
        if (!node->left || node->left->type != NODE_IDENT) return;
        const char* name = node->left->str_val;
        Value val = eval_expr(node->right, env, ctx);
        if (ctx->constraint_violated) return;

        int ret = env_set(env, name, val);
        if (ret < 0) {
#if CONFIG_STRICT_MODE
            ctx->constraint_violated = true;
            ctx->violation_msg       = "Undefined variable";
#endif
        }
        return;
    }

    case NODE_IF: {
        Value cond = eval_expr(node->condition, env, ctx);
        if (ctx->constraint_violated) return;

        if (is_truthy(cond)) {
            execute_statement(node->if_body, env, ctx);
        } else if (node->else_body) {
            execute_statement(node->else_body, env, ctx);
        }
        return;
    }

    case NODE_WHILE: {
        if (ctx->loop_depth >= MAX_NESTED_LOOPS) {
            ctx->constraint_violated = true;
            ctx->violation_msg       = "Nested loop limit exceeded";
            return;
        }

        int depth = ctx->loop_depth++;
        ctx->loop_iterations[depth] = 0;

        // Save/restore inside_loop so nested whiles don't clobber it (B1)
        bool saved_inside = ctx->inside_loop;
        ctx->inside_loop = true;

        while (1) {
            if (ctx->constraint_violated || ctx->has_returned) break;
            // Memory barrier ensures cross-core visibility of watchdog flag
            __sync_synchronize();
            if (ctx->s_script_timeout_ptr &&
                *ctx->s_script_timeout_ptr) {
                ctx->constraint_violated = true;
                ctx->violation_msg       = "Script execution timeout";
                break;
            }

            Value cond = eval_expr(node->test, env, ctx);
            if (ctx->constraint_violated) break;
            if (!is_truthy(cond)) break;

            ctx->loop_iterations[depth]++;
            if (ctx->loop_iterations[depth] >
                CONFIG_MAX_LOOP_ITERATIONS) {
                ctx->constraint_violated = true;
                ctx->violation_msg =
                    "Loop iteration limit exceeded";
                break;
            }

            execute_statement(node->body, env, ctx);
        }

        ctx->inside_loop = saved_inside;
        ctx->loop_depth--;
        return;
    }

    case NODE_BLOCK: {
        ctx->exec_depth++;
        if (ctx->exec_depth > CONFIG_MAX_EXEC_DEPTH) {
            ctx->constraint_violated = true;
            ctx->violation_msg       = "Maximum execution depth exceeded";
            ctx->exec_depth--;
            return;
        }
        execute_block_stmts(node->stmts, node->stmt_count, env, ctx);
        ctx->exec_depth--;
        return;
    }

    case NODE_FUNC_DEF: {
        FuncObj* func = func_alloc();
        if (!func) {
            ctx->constraint_violated = true;
            ctx->violation_msg       = "Function pool exhausted";
            return;
        }
        func->name        = node->func_name;
        func->param_count = node->param_count;
        func->body        = node->func_body;

        for (int i = 0;
             i < node->param_count && i < CONFIG_MAX_FUNC_PARAMS;
             i++) {
            func->params[i] = node->params[i];
        }

        Value fn_val;
        fn_val.type = VAL_FUNC;
        fn_val.func = func;

        int ret = env_define(env, func->name, fn_val);
        if (ret < 0) {
            ctx->constraint_violated = true;
            ctx->violation_msg       = "Environment full";
        }
        return;
    }

    case NODE_RETURN_STMT: {
        if (node->left) {
            ctx->return_value =
                eval_expr(node->left, env, ctx);
        } else {
            ctx->return_value = val_undefined();
        }
        ctx->has_returned = true;
        return;
    }

    // Expression statements --- evaluate and discard
    case NODE_FUNC_CALL:
    default: {
        Value result = eval_expr(node, env, ctx);
        (void)result;
        return;
    }
    }
}

// ====================================================================
// Block execution --- iterates statements with runtime constraint checks
// ====================================================================

static void execute_block_stmts(ASTNode** stmts, int stmt_count,
                                Environment* env, ExecutionContext* ctx)
{
    for (int i = 0; i < stmt_count; i++) {
        if (ctx->constraint_violated || ctx->has_returned) return;

        // G6: Detect consecutive remote_read calls
        if (stmts[i]->type == NODE_FUNC_CALL &&
            stmts[i]->name != NULL &&
            ctx->last_call_name != NULL &&
            strcmp(ctx->last_call_name, "remote_read") == 0 &&
            strcmp(stmts[i]->name, "remote_read") == 0) {
            ESP_LOGW(TAG, "Consecutive remote_read calls detected "
                          "at line %d — consider using aggregation functions",
                     stmts[i]->line);
        }

        // Memory barrier ensures cross-core visibility of watchdog flag
        __sync_synchronize();
        if (ctx->s_script_timeout_ptr &&
            *ctx->s_script_timeout_ptr) {
            ctx->constraint_violated = true;
            ctx->violation_msg       = "Script execution timeout";
            return;
        }

        ctx->total_statements++;
        if (ctx->total_statements > CONFIG_MAX_EXEC_STATEMENTS) {
            ctx->constraint_violated = true;
            ctx->violation_msg       = "Statement limit exceeded";
            return;
        }

        execute_statement(stmts[i], env, ctx);

        // G6: Track last call name for consecutive detection
        if (stmts[i]->type == NODE_FUNC_CALL) {
            ctx->last_call_name = stmts[i]->name;
        } else {
            ctx->last_call_name = NULL;
        }
    }
}

// ====================================================================
// Function call --- user-defined and builtin dispatch
// ====================================================================

static Value call_function(const char* name, ASTNode** args, int arg_count,
                           Environment* env, ExecutionContext* ctx)
{
    // G7: Detect list_new() inside while loop
    if (ctx->inside_loop && strcmp(name, "list_new") == 0) {
        ESP_LOGW(TAG, "list_new() inside while loop — consider allocating lists outside the loop");
    }

    // Look up the function in the environment
    Value fn_val = env_get(env, name);

    // If not found in env, try builtin dispatch as fallback.
    // This works even before register_builtins() populates the env (P6).
    if (fn_val.type != VAL_FUNC || !fn_val.func) {
        Value arg_values[16];
        int n = (arg_count < 16) ? arg_count : 16;
        for (int i = 0; i < n; i++) {
            arg_values[i] = eval_expr(args[i], env, ctx);
            if (ctx->constraint_violated) return val_undefined();
        }
        return call_builtin(name, arg_values, n, ctx);
    }

    FuncObj* func = fn_val.func;

    // Builtin function (body == NULL, registered via register_builtins)
    if (func->body == NULL) {
        Value arg_values[16];
        int n = (arg_count < 16) ? arg_count : 16;
        for (int i = 0; i < n; i++) {
            arg_values[i] = eval_expr(args[i], env, ctx);
            if (ctx->constraint_violated) return val_undefined();
        }
        return call_builtin(name, arg_values, n, ctx);
    }

    // --- User-defined function ---

    // Argument count is always strict (per design doc section 6.9.1)
    if (func->param_count != arg_count) {
        ctx->constraint_violated = true;
        ctx->violation_msg       = "Argument count mismatch";
        return val_undefined();
    }

    // Execution depth check
    ctx->exec_depth++;
    if (ctx->exec_depth > CONFIG_MAX_EXEC_DEPTH) {
        ctx->constraint_violated = true;
        ctx->violation_msg       = "Execution depth exceeded";
        ctx->exec_depth--;
        return val_undefined();
    }

    // Allocate local environment for the function scope
    Environment* local_env = env_alloc(env);
    if (!local_env) {
        ctx->constraint_violated = true;
        ctx->violation_msg       = "Function nesting depth exceeded";
        ctx->exec_depth--;
        return val_undefined();
    }
    ctx->env_pool_used++;

    // Bind parameters -- evaluate args in the CALLER's environment
    for (int i = 0; i < func->param_count; i++) {
        Value arg_val = eval_expr(args[i], env, ctx);
        if (ctx->constraint_violated) {
            env_free(local_env);
            ctx->env_pool_used--;
            ctx->exec_depth--;
            return val_undefined();
        }
        env_define(local_env, func->params[i], arg_val);
    }

    // Execute function body. execute() consumes has_returned
    // and returns the result value.
    ctx->has_returned = false;
    Value result = execute(func->body, local_env, ctx);

    // Clean up
    env_free(local_env);
    ctx->env_pool_used--;
    ctx->exec_depth--;

    return result;
}

// ====================================================================
// Builtin function dispatch --- delegates to builtins.cpp (P6)
// ====================================================================

static Value call_builtin(const char* name, const Value* args,
                          int arg_count, ExecutionContext* ctx)
{
    return call_builtin_by_name(name, args, arg_count, ctx);
}

// ====================================================================
// Public API --- execute an AST (entry point)
// ====================================================================

Value execute(ASTNode* ast, Environment* env, ExecutionContext* ctx)
{
    if (!ast || !env || !ctx) {
        return val_undefined();
    }

    // Execute program or block statements
    if (ast->type == NODE_PROGRAM || ast->type == NODE_BLOCK) {
        execute_block_stmts(ast->stmts, ast->stmt_count, env, ctx);
    } else {
        // Single statement path
        execute_statement(ast, env, ctx);
    }

    // Consume return value if one was set
    if (ctx->has_returned) {
        Value v = ctx->return_value;
        ctx->has_returned = false;
        return v;
    }

    return val_undefined();
}

// ====================================================================
// Tree depth computation helper for validate_resources
// ====================================================================

static int compute_tree_depth(const ASTNode* node)
{
    if (!node) return 0;

    switch (node->type) {

    case NODE_PROGRAM:
    case NODE_BLOCK: {
        int max_depth = 1;
        for (int i = 0; i < node->stmt_count; i++) {
            int d = 1 + compute_tree_depth(node->stmts[i]);
            if (d > max_depth) max_depth = d;
        }
        return max_depth;
    }

    case NODE_VAR_DECL:
        return 1 + compute_tree_depth(node->func_body);

    case NODE_ASSIGN: {
        int ld = compute_tree_depth(node->left);
        int rd = compute_tree_depth(node->right);
        return 1 + (ld > rd ? ld : rd);
    }

    case NODE_IF: {
        int cd = compute_tree_depth(node->condition);
        int bd = compute_tree_depth(node->if_body);
        int ed = node->else_body
                     ? compute_tree_depth(node->else_body)
                     : 0;
        int max = (cd > bd) ? cd : bd;
        max = (ed > max) ? ed : max;
        return 1 + max;
    }

    case NODE_WHILE: {
        int td = compute_tree_depth(node->test);
        int bd = compute_tree_depth(node->body);
        return 1 + (td > bd ? td : bd);
    }

    case NODE_BINARY_OP: {
        int ld = compute_tree_depth(node->left);
        int rd = compute_tree_depth(node->right);
        return 1 + (ld > rd ? ld : rd);
    }

    case NODE_UNARY_OP:
        return 1 + compute_tree_depth(node->left);

    case NODE_LITERAL_NUM:
    case NODE_LITERAL_STR:
    case NODE_LITERAL_BOOL:
    case NODE_IDENT:
        return 1;

    case NODE_FUNC_CALL: {
        int max_depth = 1;
        for (int i = 0; i < node->arg_count; i++) {
            int d = 1 + compute_tree_depth(node->args[i]);
            if (d > max_depth) max_depth = d;
        }
        return max_depth;
    }

    case NODE_FUNC_DEF:
        return 1 + compute_tree_depth(node->func_body);

    case NODE_RETURN_STMT:
        return 1 + compute_tree_depth(node->left);

    default:
        return 1;
    }
}

// ====================================================================
// walk_validate — recursive AST scanner for resource counting
// ====================================================================

static void walk_validate(const ASTNode* node, WalkState* ws, int depth)
{
    if (!node || !ws) return;
    if (depth > ws->max_depth) ws->max_depth = depth;

    switch (node->type) {

    case NODE_PROGRAM:
    case NODE_BLOCK:
        for (int i = 0; i < node->stmt_count; i++)
            walk_validate(node->stmts[i], ws, depth + 1);
        break;

    case NODE_VAR_DECL:
        walk_validate(node->func_body, ws, depth + 1);
        break;

    case NODE_ASSIGN:
        walk_validate(node->left,  ws, depth + 1);
        walk_validate(node->right, ws, depth + 1);
        break;

    case NODE_IF:
        walk_validate(node->condition,  ws, depth + 1);
        walk_validate(node->if_body,    ws, depth + 1);
        if (node->else_body) walk_validate(node->else_body, ws, depth + 1);
        break;

    case NODE_WHILE: {
        walk_validate(node->test, ws, depth + 1);
        bool saved = ws->inside_while;
        ws->inside_while = true;
        walk_validate(node->body, ws, depth + 1);
        ws->inside_while = saved;
        break;
    }

    case NODE_BINARY_OP:
        walk_validate(node->left,  ws, depth + 1);
        walk_validate(node->right, ws, depth + 1);
        break;

    case NODE_UNARY_OP:
        walk_validate(node->left, ws, depth + 1);
        break;

    case NODE_FUNC_CALL:
        for (int i = 0; i < node->arg_count; i++)
            walk_validate(node->args[i], ws, depth + 1);
        if (node->name && strcmp(node->name, "list_new") == 0) {
            if (node->arg_count > 0 && node->args[0] &&
                node->args[0]->type == NODE_LITERAL_NUM) {
                ws->list_elements_total += (int)node->args[0]->num_val;
            } else {
                ws->list_elements_total += 1;
            }
            if (ws->inside_while) ws->has_list_in_loop = true;
        }
        break;

    case NODE_FUNC_DEF:
        ws->funcs_defined++;
        walk_validate(node->func_body, ws, depth + 1);
        break;

    case NODE_RETURN_STMT:
        walk_validate(node->left, ws, depth + 1);
        break;

    case NODE_LITERAL_NUM:
    case NODE_LITERAL_STR:
    case NODE_LITERAL_BOOL:
    case NODE_IDENT:
    default:
        break;
    }
}

// ====================================================================
// validate_resources — AST resource usage report (design.md §15.3)
// ====================================================================

ResourceReport validate_resources(ASTNode* ast, Environment* env)
{
    ResourceReport r;
    memset(&r, 0, sizeof(r));
    r.passed = true;
    r.ast_node_count = ast_pool_used();
    r.ast_tree_depth = compute_tree_depth(ast);
    int env_bindings = (env) ? env->count : 0;
    int builtin_bindings = (env_bindings >= BIF_COUNT) ? BIF_COUNT : 0;
    r.global_bindings_used = env_bindings - builtin_bindings;

    WalkState ws;
    memset(&ws, 0, sizeof(ws));
    walk_validate(ast, &ws, 1);
    r.funcs_defined       = ws.funcs_defined;
    r.list_elements_total = ws.list_elements_total;
    r.has_list_in_loop    = ws.has_list_in_loop;
    r.max_parse_depth     = ws.max_depth;

    // Threshold checks (design.md §15.3)
    if (r.ast_node_count > CONFIG_AST_POOL_SIZE * 90 / 100) {
        r.fail_reason = "AST pool > 90%, maximum parse depth/nodes reached";
        r.passed = false;
    }

    if (r.funcs_defined > CONFIG_FUNC_POOL_SIZE * 90 / 100) {
        r.fail_reason = "User function count > 90% pool";
        r.passed = false;
    }

    if (r.global_bindings_used > CONFIG_MAX_BINDINGS * 80 / 100) {
        r.fail_reason = "Global bindings > 80% capacity";
        r.passed = false;
    }

    if (r.ast_tree_depth > CONFIG_MAX_PARSE_DEPTH * 80 / 100) {
        r.fail_reason = "AST tree depth > 80% max";
        r.passed = false;
    }

    if (r.list_elements_total > 50) {
        ESP_LOGW(TAG, "Total list elements (%d) > 50", r.list_elements_total);
    }

    if (r.has_list_in_loop) {
        ESP_LOGW(TAG, "list_new() inside while loop — consider allocating outside");
    }

    return r;
}
