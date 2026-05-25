#include "sdkconfig.h"
#include "interpreter/parser.h"
#include "interpreter/intern.h"
#include <string.h>

// ---------- compile-time defaults (when sdkconfig.h missing during dev) ----------

#ifndef CONFIG_MAX_PARSE_DEPTH
#define CONFIG_MAX_PARSE_DEPTH 32
#endif

#ifndef CONFIG_MAX_FUNC_PARAMS
#define CONFIG_MAX_FUNC_PARAMS 8
#endif

// ---------- parser limits ----------

#define MAX_STMTS       256      // max statements per program / block
#define MAX_CALL_ARGS   16      // max function-call arguments
#define MAX_STMT_POOL   1024    // total ASTNode* pointers for all stmt/arg arrays
#define MAX_PARAM_ENTRIES 128   // total const char* slots for func-decl param arrays
                                // (16 func defs × 8 params each = 128)

// ---------- statement-pointer pool ----------
// Static pool of ASTNode* pointers for NODE_PROGRAM / NODE_BLOCK stmts arrays
// AND for function-call argument arrays (parse_call).  Reset per parse.

static ASTNode* s_ptr_pool[MAX_STMT_POOL];
static int      s_ptr_pool_used = 0;

static void reset_ptr_pool(void)
{
    s_ptr_pool_used = 0;
}

static ASTNode** alloc_ptrs(int count)
{
    if (s_ptr_pool_used + count > MAX_STMT_POOL) return NULL;
    ASTNode** result = &s_ptr_pool[s_ptr_pool_used];
    s_ptr_pool_used += count;
    return result;
}

// ---------- function-parameter pool ----------
// Each parse_func_decl needs a persistent array of const char* param names.
// This pool provides that storage across the full parse.

static const char* s_param_pool[MAX_PARAM_ENTRIES];
static int         s_param_pool_used = 0;

static void reset_param_pool(void)
{
    s_param_pool_used = 0;
}

static const char** alloc_params(int count)
{
    if (s_param_pool_used + count > MAX_PARAM_ENTRIES) return NULL;
    const char** result = &s_param_pool[s_param_pool_used];
    s_param_pool_used += count;
    return result;
}

// ---------- forward declarations ----------

static ASTNode* parse_statement(Parser* p);
static ASTNode* parse_var_decl(Parser* p);
static ASTNode* parse_if(Parser* p);
static ASTNode* parse_while(Parser* p);
static ASTNode* parse_block(Parser* p);
static ASTNode* parse_func_decl(Parser* p);
static ASTNode* parse_return(Parser* p);
static ASTNode* parse_expression_statement(Parser* p);
static ASTNode* parse_assignment(Parser* p);
static ASTNode* parse_logic_or(Parser* p);
static ASTNode* parse_logic_and(Parser* p);
static ASTNode* parse_equality(Parser* p);
static ASTNode* parse_comparison(Parser* p);
static ASTNode* parse_term(Parser* p);
static ASTNode* parse_factor(Parser* p);
static ASTNode* parse_unary(Parser* p);
static ASTNode* parse_call(Parser* p);
static ASTNode* parse_primary(Parser* p);
static ASTNode* parse_expression(Parser* p);

// ---------- helpers ----------

static Token advance(Parser* p)
{
    Token t = p->current;
    p->current = lexer_next(p->lexer);
    return t;
}

static bool check(Parser* p, TokenType type)
{
    return p->current.type == type;
}

static bool match(Parser* p, TokenType type)
{
    if (check(p, type)) {
        advance(p);
        return true;
    }
    return false;
}

static void error(Parser* p, const char* msg)
{
    if (p->had_error) return;           // keep first error
    p->had_error    = true;
    p->error_msg    = msg;
    p->error_line   = p->current.line;
    p->error_col    = p->current.col;
}

static bool expect(Parser* p, TokenType type, const char* msg)
{
    if (check(p, type)) {
        advance(p);
        return true;
    }
    error(p, msg);
    return false;
}

// Error recovery: skip tokens until a statement-boundary token.
// Consume a trailing semicolon so the next statement starts clean.
// Leave RBRACE for the block parser to consume.
static void sync(Parser* p)
{
    while (!check(p, TOKEN_SEMICOLON) &&
           !check(p, TOKEN_RBRACE) &&
           !check(p, TOKEN_END)) {
        advance(p);
    }
    if (check(p, TOKEN_SEMICOLON)) {
        advance(p);
    }
}

// ---------- parse_program ----------

static ASTNode* parse_program(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    ASTNode* node = ast_alloc_node();
    if (!node) {
        error(p, "AST pool exhausted");
        p->parse_depth--;
        return NULL;
    }
    node->type       = NODE_PROGRAM;
    node->line       = 1;
    node->col        = 1;
    node->op         = (TokenType)0;
    node->stmts      = NULL;
    node->stmt_count = 0;

    ASTNode** stmts = alloc_ptrs(MAX_STMTS);
    if (!stmts) {
        error(p, "Statement pointer pool exhausted");
        p->parse_depth--;
        return NULL;
    }

    int count = 0;
    while (!check(p, TOKEN_END) && !p->had_error) {
        ASTNode* stmt = parse_statement(p);
        if (stmt) {
            stmts[count++] = stmt;
            if (count >= MAX_STMTS) break;
        }
    }

    node->stmts      = stmts;
    node->stmt_count = count;

    p->parse_depth--;
    return node;
}

// ---------- parse_statement ----------

static ASTNode* parse_statement(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    ASTNode* node = NULL;

    switch (p->current.type) {
        case TOKEN_VAR:
            node = parse_var_decl(p);
            break;
        case TOKEN_IF:
            node = parse_if(p);
            break;
        case TOKEN_WHILE:
            node = parse_while(p);
            break;
        case TOKEN_LBRACE:
            node = parse_block(p);
            break;
        case TOKEN_FUNC:
            node = parse_func_decl(p);
            break;
        case TOKEN_RETURN:
            node = parse_return(p);
            break;
        case TOKEN_SEMICOLON:
            advance(p);               // empty statement
            node = NULL;
            break;
        default:
            node = parse_expression_statement(p);
            break;
    }

    p->parse_depth--;
    return node;
}

// ---------- parse_var_decl ----------

static ASTNode* parse_var_decl(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    advance(p);                         // consume TOKEN_VAR

    if (!check(p, TOKEN_IDENTIFIER)) {
        error(p, "Expected variable name");
        sync(p);
        p->parse_depth--;
        return NULL;
    }
    Token name_tok = advance(p);
    const char* name = name_tok.str;

    if (!expect(p, TOKEN_ASSIGN, "Expected '=' in variable declaration")) {
        sync(p);
        p->parse_depth--;
        return NULL;
    }

    ASTNode* value = parse_assignment(p);
    if (!value) {
        if (!p->had_error) {
            error(p, "Expected expression in variable declaration");
        }
        sync(p);
        p->parse_depth--;
        return NULL;
    }

    if (!expect(p, TOKEN_SEMICOLON, "Expected ';' after variable declaration")) {
        sync(p);
        p->parse_depth--;
        return NULL;
    }

    ASTNode* node = ast_alloc_node();
    if (!node) {
        error(p, "AST pool exhausted");
        p->parse_depth--;
        return NULL;
    }
    node->type      = NODE_VAR_DECL;
    node->line      = name_tok.line;
    node->col       = name_tok.col;
    node->op        = (TokenType)0;
    // VAR_DECL uses func_name (name) + func_body (initializer) from the union
    node->func_name = name;
    node->func_body = value;

    p->parse_depth--;
    return node;
}

// ---------- parse_if ----------

static ASTNode* parse_if(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    Token if_tok = advance(p);          // consume TOKEN_IF

    if (!expect(p, TOKEN_LPAREN, "Expected '(' after 'if'")) {
        sync(p);
        p->parse_depth--;
        return NULL;
    }

    ASTNode* condition = parse_expression(p);
    if (!condition) {
        sync(p);
        p->parse_depth--;
        return NULL;
    }

    if (!expect(p, TOKEN_RPAREN, "Expected ')' after if condition")) {
        sync(p);
        p->parse_depth--;
        return NULL;
    }

    ASTNode* if_body = parse_statement(p);

    ASTNode* else_body = NULL;
    if (match(p, TOKEN_ELSE)) {
        else_body = parse_statement(p);
    }

    ASTNode* node = ast_alloc_node();
    if (!node) {
        error(p, "AST pool exhausted");
        p->parse_depth--;
        return NULL;
    }
    node->type      = NODE_IF;
    node->line      = if_tok.line;
    node->col       = if_tok.col;
    node->op        = (TokenType)0;
    node->condition = condition;
    node->if_body   = if_body;
    node->else_body = else_body;

    p->parse_depth--;
    return node;
}

// ---------- parse_while ----------

static ASTNode* parse_while(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    Token while_tok = advance(p);       // consume TOKEN_WHILE

    if (!expect(p, TOKEN_LPAREN, "Expected '(' after 'while'")) {
        sync(p);
        p->parse_depth--;
        return NULL;
    }

    ASTNode* condition = parse_expression(p);
    if (!condition) {
        sync(p);
        p->parse_depth--;
        return NULL;
    }

    if (!expect(p, TOKEN_RPAREN, "Expected ')' after while condition")) {
        sync(p);
        p->parse_depth--;
        return NULL;
    }

    ASTNode* body = parse_statement(p);

    ASTNode* node = ast_alloc_node();
    if (!node) {
        error(p, "AST pool exhausted");
        p->parse_depth--;
        return NULL;
    }
    node->type  = NODE_WHILE;
    node->line  = while_tok.line;
    node->col   = while_tok.col;
    node->op    = (TokenType)0;
    // WHILE stores condition in test, body in body, init/update unused
    node->init  = NULL;
    node->test  = condition;
    node->update = NULL;
    node->body  = body;

    p->parse_depth--;
    return node;
}

// ---------- parse_block ----------

static ASTNode* parse_block(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    Token open_tok = advance(p);        // consume TOKEN_LBRACE

    ASTNode** stmts = alloc_ptrs(MAX_STMTS);
    if (!stmts) {
        error(p, "Statement pointer pool exhausted");
        p->parse_depth--;
        return NULL;
    }

    int count = 0;
    while (!check(p, TOKEN_RBRACE) && !check(p, TOKEN_END) && !p->had_error) {
        ASTNode* stmt = parse_statement(p);
        if (stmt) {
            stmts[count++] = stmt;
            if (count >= MAX_STMTS) break;
        }
    }

    if (!expect(p, TOKEN_RBRACE, "Expected '}' to close block")) {
        if (!check(p, TOKEN_END)) sync(p);
    }

    ASTNode* node = ast_alloc_node();
    if (!node) {
        error(p, "AST pool exhausted");
        p->parse_depth--;
        return NULL;
    }
    node->type       = NODE_BLOCK;
    node->line       = open_tok.line;
    node->col        = open_tok.col;
    node->op         = (TokenType)0;
    node->stmts      = stmts;
    node->stmt_count = count;

    p->parse_depth--;
    return node;
}

// ---------- parse_func_decl ----------

static ASTNode* parse_func_decl(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    Token func_tok = advance(p);        // consume TOKEN_FUNC

    if (!check(p, TOKEN_IDENTIFIER)) {
        error(p, "Expected function name");
        sync(p);
        p->parse_depth--;
        return NULL;
    }
    Token name_tok = advance(p);
    const char* func_name = name_tok.str;

    if (!expect(p, TOKEN_LPAREN, "Expected '(' after function name")) {
        sync(p);
        p->parse_depth--;
        return NULL;
    }

    // Allocate persistent param array from the param pool
    const char** params = alloc_params(CONFIG_MAX_FUNC_PARAMS);
    if (!params) {
        error(p, "Parameter pool exhausted");
        p->parse_depth--;
        return NULL;
    }
    int param_count = 0;

    if (!check(p, TOKEN_RPAREN)) {
        do {
            if (param_count >= CONFIG_MAX_FUNC_PARAMS) {
                error(p, "Too many function parameters");
                sync(p);
                p->parse_depth--;
                return NULL;
            }
            if (!check(p, TOKEN_IDENTIFIER)) {
                error(p, "Expected parameter name");
                sync(p);
                p->parse_depth--;
                return NULL;
            }
            Token param_tok = advance(p);
            params[param_count++] = param_tok.str;
        } while (match(p, TOKEN_COMMA));
    }

    if (!expect(p, TOKEN_RPAREN, "Expected ')' after function parameters")) {
        sync(p);
        p->parse_depth--;
        return NULL;
    }

    ASTNode* body = parse_block(p);     // function body must be a block

    ASTNode* node = ast_alloc_node();
    if (!node) {
        error(p, "AST pool exhausted");
        p->parse_depth--;
        return NULL;
    }
    node->type       = NODE_FUNC_DEF;
    node->line       = func_tok.line;
    node->col        = func_tok.col;
    node->op         = (TokenType)0;
    node->func_name  = func_name;
    node->func_body  = body;
    node->params     = params;
    node->param_count = param_count;

    p->parse_depth--;
    return node;
}

// ---------- parse_return ----------

static ASTNode* parse_return(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    Token ret_tok = advance(p);         // consume TOKEN_RETURN

    ASTNode* value = parse_expression(p);
    if (!value) {
        if (!p->had_error) {
            error(p, "Expected expression after 'return'");
        }
        sync(p);
        p->parse_depth--;
        return NULL;
    }

    if (!expect(p, TOKEN_SEMICOLON, "Expected ';' after return value")) {
        sync(p);
        p->parse_depth--;
        return NULL;
    }

    ASTNode* node = ast_alloc_node();
    if (!node) {
        error(p, "AST pool exhausted");
        p->parse_depth--;
        return NULL;
    }
    node->type  = NODE_RETURN_STMT;
    node->line  = ret_tok.line;
    node->col   = ret_tok.col;
    node->op    = (TokenType)0;
    // Return value stored in {left, right}
    node->left  = value;
    node->right = NULL;

    p->parse_depth--;
    return node;
}

// ---------- parse_expression_statement ----------

static ASTNode* parse_expression_statement(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    ASTNode* expr = parse_expression(p);
    if (!expr) {
        sync(p);
        p->parse_depth--;
        return NULL;
    }

    if (!expect(p, TOKEN_SEMICOLON, "Expected ';' after expression")) {
        sync(p);
    }

    p->parse_depth--;
    return expr;
}

// ---------- parse_expression (alias for parse_assignment) ----------

static ASTNode* parse_expression(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    ASTNode* node = parse_assignment(p);

    p->parse_depth--;
    return node;
}

// ---------- parse_assignment ----------

static ASTNode* parse_assignment(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    ASTNode* expr = parse_logic_or(p);
    if (!expr) {
        p->parse_depth--;
        return NULL;
    }

    if (match(p, TOKEN_ASSIGN)) {
        if (expr->type != NODE_IDENT) {
            error(p, "Invalid assignment target (must be identifier)");
            p->parse_depth--;
            return NULL;
        }

        ASTNode* value = parse_assignment(p);
        if (!value) {
            p->parse_depth--;
            return NULL;
        }

        ASTNode* node = ast_alloc_node();
        if (!node) {
            error(p, "AST pool exhausted");
            p->parse_depth--;
            return NULL;
        }
        node->type  = NODE_ASSIGN;
        node->line  = expr->line;
        node->col   = expr->col;
        node->op    = (TokenType)0;
        node->left  = expr;     // the NODE_IDENT
        node->right = value;

        p->parse_depth--;
        return node;
    }

    p->parse_depth--;
    return expr;
}

// ---------- binary-op left-associative loop helper ----------

static ASTNode* parse_binary_left(Parser* p,
                                  ASTNode* (*sub)(Parser*),
                                  const TokenType* ops, int op_count)
{
    ASTNode* left = sub(p);
    if (!left) return NULL;

    while (1) {
        TokenType op = (TokenType)0;
        bool found = false;
        for (int i = 0; i < op_count; i++) {
            if (check(p, ops[i])) {
                op = ops[i];
                found = true;
                break;
            }
        }
        if (!found) break;

        advance(p);  // consume operator token

        ASTNode* right = sub(p);
        if (!right) return NULL;

        ASTNode* node = ast_alloc_node();
        if (!node) {
            error(p, "AST pool exhausted");
            return NULL;
        }
        node->type  = NODE_BINARY_OP;
        node->op    = op;
        node->line  = left->line;
        node->col   = left->col;
        node->left  = left;
        node->right = right;
        left = node;
    }

    return left;
}

// ---------- parse_logic_or ----------

static ASTNode* parse_logic_or(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    ASTNode* left = parse_logic_and(p);
    if (!left) {
        p->parse_depth--;
        return NULL;
    }

    while (match(p, TOKEN_OR)) {
        ASTNode* right = parse_logic_and(p);
        if (!right) {
            p->parse_depth--;
            return NULL;
        }

        ASTNode* node = ast_alloc_node();
        if (!node) {
            error(p, "AST pool exhausted");
            p->parse_depth--;
            return NULL;
        }
        node->type  = NODE_BINARY_OP;
        node->op    = TOKEN_OR;
        node->line  = left->line;
        node->col   = left->col;
        node->left  = left;
        node->right = right;
        left = node;
    }

    p->parse_depth--;
    return left;
}

// ---------- parse_logic_and ----------

static ASTNode* parse_logic_and(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    ASTNode* left = parse_equality(p);
    if (!left) {
        p->parse_depth--;
        return NULL;
    }

    while (match(p, TOKEN_AND)) {
        ASTNode* right = parse_equality(p);
        if (!right) {
            p->parse_depth--;
            return NULL;
        }

        ASTNode* node = ast_alloc_node();
        if (!node) {
            error(p, "AST pool exhausted");
            p->parse_depth--;
            return NULL;
        }
        node->type  = NODE_BINARY_OP;
        node->op    = TOKEN_AND;
        node->line  = left->line;
        node->col   = left->col;
        node->left  = left;
        node->right = right;
        left = node;
    }

    p->parse_depth--;
    return left;
}

// ---------- parse_equality ----------

static ASTNode* parse_equality(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    static const TokenType ops[] = { TOKEN_EQ, TOKEN_NEQ };
    ASTNode* node = parse_binary_left(p, parse_comparison, ops, 2);

    p->parse_depth--;
    return node;
}

// ---------- parse_comparison ----------

static ASTNode* parse_comparison(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    static const TokenType ops[] = { TOKEN_LT, TOKEN_GT, TOKEN_LE, TOKEN_GE };
    ASTNode* node = parse_binary_left(p, parse_term, ops, 4);

    p->parse_depth--;
    return node;
}

// ---------- parse_term ----------

static ASTNode* parse_term(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    static const TokenType ops[] = { TOKEN_PLUS, TOKEN_MINUS };
    ASTNode* node = parse_binary_left(p, parse_factor, ops, 2);

    p->parse_depth--;
    return node;
}

// ---------- parse_factor ----------

static ASTNode* parse_factor(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    static const TokenType ops[] = { TOKEN_STAR, TOKEN_SLASH };
    ASTNode* node = parse_binary_left(p, parse_unary, ops, 2);

    p->parse_depth--;
    return node;
}

// ---------- parse_unary ----------

static ASTNode* parse_unary(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    if (check(p, TOKEN_NOT) || check(p, TOKEN_MINUS)) {
        TokenType op = p->current.type;
        Token op_tok = advance(p);

        ASTNode* operand = parse_unary(p);
        if (!operand) {
            p->parse_depth--;
            return NULL;
        }

        ASTNode* node = ast_alloc_node();
        if (!node) {
            error(p, "AST pool exhausted");
            p->parse_depth--;
            return NULL;
        }
        node->type  = NODE_UNARY_OP;
        node->op    = op;
        node->line  = op_tok.line;
        node->col   = op_tok.col;
        node->left  = operand;
        node->right = NULL;

        p->parse_depth--;
        return node;
    }

    // Fall through to call
    ASTNode* node = parse_call(p);

    p->parse_depth--;
    return node;
}

// ---------- parse_call ----------

static ASTNode* parse_call(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    ASTNode* expr = parse_primary(p);
    if (!expr) {
        p->parse_depth--;
        return NULL;
    }

    // If followed by '(' it's a function call
    if (match(p, TOKEN_LPAREN)) {
        if (expr->type != NODE_IDENT) {
            error(p, "Invalid function call (not an identifier)");
            sync(p);
            p->parse_depth--;
            return NULL;
        }

        const char* func_name = expr->str_val;

        // Allocate args array from the ptr pool (persists beyond this call)
        ASTNode** args = alloc_ptrs(MAX_CALL_ARGS);
        if (!args) {
            error(p, "Argument pointer pool exhausted");
            sync(p);
            p->parse_depth--;
            return NULL;
        }
        int arg_count = 0;

        if (!check(p, TOKEN_RPAREN)) {
            do {
                if (arg_count >= MAX_CALL_ARGS) {
                    error(p, "Too many function arguments");
                    sync(p);
                    p->parse_depth--;
                    return NULL;
                }
                ASTNode* arg = parse_assignment(p);
                if (!arg) {
                    sync(p);
                    p->parse_depth--;
                    return NULL;
                }
                args[arg_count++] = arg;
            } while (match(p, TOKEN_COMMA));
        }

        if (!expect(p, TOKEN_RPAREN, "Expected ')' after function arguments")) {
            sync(p);
            p->parse_depth--;
            return NULL;
        }

        ASTNode* node = ast_alloc_node();
        if (!node) {
            error(p, "AST pool exhausted");
            p->parse_depth--;
            return NULL;
        }
        node->type      = NODE_FUNC_CALL;
        node->line      = expr->line;
        node->col       = expr->col;
        node->op        = (TokenType)0;
        node->name      = func_name;
        node->args      = args;
        node->arg_count = arg_count;

        p->parse_depth--;
        return node;
    }

    p->parse_depth--;
    return expr;
}

// ---------- parse_primary ----------

static ASTNode* parse_primary(Parser* p)
{
    p->parse_depth++;
    if (p->parse_depth > CONFIG_MAX_PARSE_DEPTH) {
        error(p, "Maximum parse depth exceeded");
        p->parse_depth--;
        return NULL;
    }

    ASTNode* node = NULL;

    switch (p->current.type) {
        case TOKEN_NUMBER: {
            Token t = advance(p);
            node = ast_alloc_node();
            if (!node) {
                error(p, "AST pool exhausted");
                p->parse_depth--;
                return NULL;
            }
            node->type    = NODE_LITERAL_NUM;
            node->line    = t.line;
            node->col     = t.col;
            node->op      = (TokenType)0;
            node->num_val = t.num;
            break;
        }
        case TOKEN_STRING: {
            Token t = advance(p);
            node = ast_alloc_node();
            if (!node) {
                error(p, "AST pool exhausted");
                p->parse_depth--;
                return NULL;
            }
            node->type    = NODE_LITERAL_STR;
            node->line    = t.line;
            node->col     = t.col;
            node->op      = (TokenType)0;
            node->str_val = t.str;
            break;
        }
        case TOKEN_TRUE:
        case TOKEN_FALSE: {
            Token t = advance(p);
            node = ast_alloc_node();
            if (!node) {
                error(p, "AST pool exhausted");
                p->parse_depth--;
                return NULL;
            }
            node->type     = NODE_LITERAL_BOOL;
            node->line     = t.line;
            node->col      = t.col;
            node->op       = (TokenType)0;
            node->bool_val = (t.type == TOKEN_TRUE);
            break;
        }
        case TOKEN_IDENTIFIER: {
            Token t = advance(p);
            node = ast_alloc_node();
            if (!node) {
                error(p, "AST pool exhausted");
                p->parse_depth--;
                return NULL;
            }
            node->type    = NODE_IDENT;
            node->line    = t.line;
            node->col     = t.col;
            node->op      = (TokenType)0;
            node->str_val = t.str;
            break;
        }
        case TOKEN_LPAREN: {
            advance(p);                   // consume '('
            node = parse_expression(p);
            if (!node) {
                p->parse_depth--;
                return NULL;
            }
            if (!expect(p, TOKEN_RPAREN, "Expected ')' after expression")) {
                p->parse_depth--;
                return NULL;
            }
            break;
        }
        case TOKEN_ERROR: {
            // Propagate lexer's error message (e.g. "Unterminated string")
            Token t = advance(p);
            error(p, t.str ? t.str : "Lexer error");
            p->parse_depth--;
            return NULL;
        }
        case TOKEN_END: {
            error(p, "Unexpected end of script");
            p->parse_depth--;
            return NULL;
        }
        default:
            error(p, "Expected expression");
            p->parse_depth--;
            return NULL;
    }

    p->parse_depth--;
    return node;
}

// ====================================================================
// Public API
// ====================================================================

void parser_init(Parser* parser, Lexer* lex)
{
    parser->lexer        = lex;
    parser->current      = lexer_next(lex);
    parser->parse_depth  = 0;
    parser->had_error    = false;
    parser->error_msg    = NULL;
    parser->error_line   = 0;
    parser->error_col    = 0;
}

ASTNode* parser_parse(Parser* parser)
{
    // Reset pools for fresh parse
    reset_ptr_pool();
    reset_param_pool();
    parser->parse_depth = 0;
    parser->had_error   = false;
    parser->error_msg   = NULL;
    parser->error_line  = 0;
    parser->error_col   = 0;

    ASTNode* ast = parse_program(parser);

    // Return partial AST even on error — caller checks had_error
    return ast;
}
