#pragma once

#include "sdkconfig.h"
#include "interpreter/value.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_BINDINGS CONFIG_MAX_BINDINGS

typedef struct {
    const char* name;  // interned
    Value value;
} Binding;

typedef struct Environment {
    struct Environment* parent;
    Binding bindings[MAX_BINDINGS];
    int count;
} Environment;

void env_init(Environment* env, Environment* parent);
int env_define(Environment* env, const char* name, Value value);
int env_set(Environment* env, const char* name, Value value);
Value env_get(Environment* env, const char* name);
bool env_exists(Environment* env, const char* name);

// Environment object pool — function call nesting
Environment* env_alloc(Environment* parent);
void env_free(Environment* env);

// Snapshot / restore for script isolation
int env_snapshot(Environment* env);
void env_restore_pristine(Environment* env);

#ifdef __cplusplus
}
#endif
