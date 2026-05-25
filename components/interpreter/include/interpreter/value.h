#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// Value types — design.md §6.8
typedef enum {
    VAL_NUM,
    VAL_STR,
    VAL_BOOL,
    VAL_LIST,
    VAL_FUNC,
    VAL_UNDEFINED,
} ValueType;

typedef struct FuncObj FuncObj;

typedef struct {
    double data[16];
    int len;
} ListData;

typedef struct {
    ValueType type;
    union {
        double num;
        const char* str;
        bool b;
        ListData* list;
        FuncObj* func;
    };
} Value;

// Function object — stored in function pool
struct FuncObj {
    const char* name;
    const char* params[CONFIG_MAX_FUNC_PARAMS];
    int param_count;
    struct ASTNode* body;  // from AST pool, NULL after reset_pool
};

#ifdef __cplusplus
}
#endif
