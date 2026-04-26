#pragma once

#include "sdkconfig.h"
#include "interpreter/lexer.h"
#include "interpreter/ast.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    Lexer* lexer;
    Token current;
    int parse_depth;
    bool had_error;
    const char* error_msg;
    int error_line;
    int error_col;
} Parser;

void parser_init(Parser* parser, Lexer* lex);
ASTNode* parser_parse(Parser* parser);

#ifdef __cplusplus
}
#endif
