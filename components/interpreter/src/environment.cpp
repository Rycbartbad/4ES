#include "sdkconfig.h"
#include "interpreter/environment.h"
#include <string.h>

// Environment object pool — function call scoping (design.md §6.9)
static Environment s_env_pool[CONFIG_ENV_POOL_SIZE];
static bool s_env_pool_used[CONFIG_ENV_POOL_SIZE];

// Pristine environment snapshot — script isolation (design.md §6.9.1)
static Environment s_pristine_env;

void env_init(Environment* env, Environment* parent)
{
    env->parent = parent;
    env->count = 0;
}

int env_define(Environment* env, const char* name, Value value)
{
    if (env->count >= MAX_BINDINGS) {
        return -1;  // full
    }
    int idx = env->count++;
    env->bindings[idx].name = name;
    env->bindings[idx].value = value;
    return idx;
}

int env_set(Environment* env, const char* name, Value value)
{
    // Search current scope first, then parent chain
    Environment* e = env;
    while (e) {
        for (int i = 0; i < e->count; i++) {
            if (e->bindings[i].name == name) {
                e->bindings[i].value = value;
                return i;
            }
        }
        e = e->parent;
    }
    return -1;  // not found
}

Value env_get(Environment* env, const char* name)
{
    Environment* e = env;
    while (e) {
        for (int i = 0; i < e->count; i++) {
            if (e->bindings[i].name == name) {
                return e->bindings[i].value;
            }
        }
        e = e->parent;
    }
    Value v;
    v.type = VAL_UNDEFINED;
    v.num = 0;
    return v;
}

bool env_exists(Environment* env, const char* name)
{
    Environment* e = env;
    while (e) {
        for (int i = 0; i < e->count; i++) {
            if (e->bindings[i].name == name) {
                return true;
            }
        }
        e = e->parent;
    }
    return false;
}

Environment* env_alloc(Environment* parent)
{
    for (int i = 0; i < CONFIG_ENV_POOL_SIZE; i++) {
        if (!s_env_pool_used[i]) {
            s_env_pool_used[i] = true;
            Environment* env = &s_env_pool[i];
            env_init(env, parent);
            return env;
        }
    }
    return NULL;  // pool exhausted
}

void env_free(Environment* env)
{
    for (int i = 0; i < CONFIG_ENV_POOL_SIZE; i++) {
        if (&s_env_pool[i] == env) {
            s_env_pool_used[i] = false;
            return;
        }
    }
}

int env_snapshot(Environment* env)
{
    s_pristine_env.parent = env->parent;
    s_pristine_env.count = env->count;
    memcpy(s_pristine_env.bindings, env->bindings,
           sizeof(Binding) * env->count);
    return 0;
}

void env_restore_pristine(Environment* env)
{
    // Guard: snapshot must have been taken first.
    // Without this, s_pristine_env.count = 0 and the restore would
    // wipe all bindings (including builtins), leaving env empty.
    if (s_pristine_env.count == 0) {
        return;  // no snapshot — nothing to restore
    }
    env->parent = s_pristine_env.parent;
    env->count = s_pristine_env.count;
    memcpy(env->bindings, s_pristine_env.bindings,
           sizeof(Binding) * s_pristine_env.count);
}
