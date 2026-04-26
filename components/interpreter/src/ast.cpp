#include "sdkconfig.h"
#include "interpreter/ast.h"
#include <string.h>

// Static AST node pool — no dynamic allocation
static ASTNode s_pool[AST_POOL_SIZE];
static int s_pool_used = 0;

void ast_pool_init(void)
{
    memset(s_pool, 0, sizeof(s_pool));
    s_pool_used = 0;
}

ASTNode* ast_alloc_node(void)
{
    if (s_pool_used >= AST_POOL_SIZE) {
        return NULL;
    }
    ASTNode* node = &s_pool[s_pool_used++];
    memset(node, 0, sizeof(ASTNode));
    return node;
}

void ast_pool_reset(void)
{
    s_pool_used = 0;
}

int ast_pool_used(void)
{
    return s_pool_used;
}
