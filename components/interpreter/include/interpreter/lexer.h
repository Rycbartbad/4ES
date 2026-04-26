#pragma once

#include "sdkconfig.h"
#include "interpreter/token.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* source;
    int pos;
    int line;
    int col;
} Lexer;

void lexer_init(Lexer* lex, const char* source);
Token lexer_next(Lexer* lex);

#ifdef __cplusplus
}
#endif
