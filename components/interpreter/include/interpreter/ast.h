#pragma once

#include "sdkconfig.h"
#include "interpreter/token.h"

#ifdef __cplusplus
extern "C" {
#endif

// AST node types — design.md §6.5
typedef enum {
    NODE_PROGRAM,
    NODE_BLOCK,
    NODE_VAR_DECL,
    NODE_ASSIGN,
    NODE_IF,
    NODE_WHILE,
    NODE_BINARY_OP,
    NODE_UNARY_OP,
    NODE_LITERAL_NUM,
    NODE_LITERAL_STR,
    NODE_LITERAL_BOOL,
    NODE_IDENT,
    NODE_FUNC_CALL,
    NODE_FUNC_DEF,
    NODE_RETURN_STMT,
} NodeType;

typedef struct ASTNode {
    NodeType type;
    int line;
    int col;
    TokenType op;    // BINARY_OP / UNARY_OP operator (discriminated by type)
    union {
        double num_val;
        const char* str_val;
        bool bool_val;
        struct { struct ASTNode* left; struct ASTNode* right; };
        struct { struct ASTNode* condition; struct ASTNode* if_body; struct ASTNode* else_body; };
        struct { struct ASTNode* init; struct ASTNode* test; struct ASTNode* update; struct ASTNode* body; };
        struct { const char* name; struct ASTNode** args; int arg_count; };
        struct { const char* func_name; struct ASTNode* func_body; const char** params; int param_count; };
        struct { struct ASTNode** stmts; int stmt_count; };
    };
} ASTNode;

// Object pool — static allocation, no malloc
#define AST_POOL_SIZE CONFIG_AST_POOL_SIZE

void ast_pool_init(void);
ASTNode* ast_alloc_node(void);
void ast_pool_reset(void);
int ast_pool_used(void);

#ifdef __cplusplus
}
#endif
