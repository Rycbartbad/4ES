#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// Script language Token types — BNF-defined subset
// design.md §6.4

typedef enum {
    // Literals
    TOKEN_NUMBER,
    TOKEN_STRING,
    TOKEN_IDENTIFIER,

    // Keywords
    TOKEN_VAR,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_WHILE,
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_FUNC,
    TOKEN_RETURN,

    // Operators
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_EQ,        // ==
    TOKEN_NEQ,       // !=
    TOKEN_LT,        // <
    TOKEN_GT,        // >
    TOKEN_LE,        // <=
    TOKEN_GE,        // >=
    TOKEN_ASSIGN,    // =
    TOKEN_NOT,       // !
    TOKEN_AND,       // &&
    TOKEN_OR,        // ||

    // Delimiters
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_SEMICOLON,
    TOKEN_COMMA,

    // Special
    TOKEN_END,       // end of input
    TOKEN_ERROR,     // lexer error
} TokenType;

typedef struct {
    TokenType type;
    int line;
    int col;
    union {
        double num;
        const char* str;  // interned string pointer
    };
} Token;

#ifdef __cplusplus
}
#endif
